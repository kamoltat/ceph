// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef __SIMPLEMESSENGER_H
#define __SIMPLEMESSENGER_H

#include "include/types.h"
#include "include/xlist.h"

#include <list>
#include <map>
using namespace std;
#include <ext/hash_map>
#include <ext/hash_set>
using namespace __gnu_cxx;

#include "common/Mutex.h"
#include "common/Cond.h"
#include "common/Thread.h"

#include "Messenger.h"
#include "Message.h"
#include "tcp.h"



/* Rank - per-process
 */
class SimpleMessenger {
public:
  struct Policy {
    bool lossy;
    bool server;

    Policy(bool l=false, bool s=false) :
      lossy(l), server(s) {}

    static Policy stateful_server() { return Policy(false, true); }
    static Policy stateless_server() { return Policy(true, true); }
    static Policy lossless_peer() { return Policy(false, false); }
    static Policy client() { return Policy(false, false); }
  };


public:
  void sigint();

private:
  class Endpoint;
  class Pipe;

  // incoming
  class Accepter : public Thread {
  public:
    SimpleMessenger *rank;
    bool done;
    int listen_sd;
    
    Accepter(SimpleMessenger *r) : rank(r), done(false), listen_sd(-1) {}
    
    void *entry();
    void stop();
    int bind(int64_t force_nonce);
    int start();
  } accepter;

  void sigint(int r);

  // pipe
  class Pipe {
  public:
    SimpleMessenger *rank;
    ostream& _pipe_prefix();

    enum {
      STATE_ACCEPTING,
      STATE_CONNECTING,
      STATE_OPEN,
      STATE_STANDBY,
      STATE_CLOSED,
      STATE_CLOSING,
      STATE_WAIT       // just wait for racing connection
    };

    int sd;
    int peer_type;
    entity_addr_t peer_addr;
    Policy policy;
    
    Mutex lock;
    int state;

  protected:
    Connection *connection_state;

    utime_t backoff;         // backoff time

    bool reader_running;
    bool writer_running;

    map<int, list<Message*> > out_q;  // priority queue for outbound msgs
    map<int, list<Message*> > in_q; // and inbound ones
    map<int, xlist<Pipe *>::item > queue_items; //put in msgr queue
    list<Message*> sent;
    Cond cond;
    bool keepalive;
    
    __u32 connect_seq, peer_global_seq;
    __u64 out_seq;
    __u64 in_seq, in_seq_acked;
    
    int accept();   // server handshake
    int connect();  // client handshake
    void reader();
    void writer();
    void unlock_maybe_reap();

    Message *read_message();
    int write_message(Message *m);
    int do_sendmsg(int sd, struct msghdr *msg, int len, bool more=false);
    int write_ack(__u64 s);
    int write_keepalive();

    void fault(bool silent=false, bool reader=false);
    void fail();

    void was_session_reset();

    // threads
    class Reader : public Thread {
      Pipe *pipe;
    public:
      Reader(Pipe *p) : pipe(p) {}
      void *entry() { pipe->reader(); return 0; }
    } reader_thread;
    friend class Reader;

    class Writer : public Thread {
      Pipe *pipe;
    public:
      Writer(Pipe *p) : pipe(p) {}
      void *entry() { pipe->writer(); return 0; }
    } writer_thread;
    friend class Writer;
    
  public:
    Pipe(SimpleMessenger *r, int st) : 
      rank(r),
      sd(-1), peer_type(-1),
      lock("SimpleMessenger::Pipe::lock"),
      state(st), 
      connection_state(new Connection),
      reader_running(false), writer_running(false),
      keepalive(false),
      connect_seq(0), peer_global_seq(0),
      out_seq(0), in_seq(0), in_seq_acked(0),
      reader_thread(this), writer_thread(this) { }
    ~Pipe() {
      assert(out_q.empty());
      assert(sent.empty());
      connection_state->put();
    }


    void start_reader() {
      reader_running = true;
      reader_thread.create();
    }
    void start_writer() {
      writer_running = true;
      writer_thread.create();
    }
    void join_reader() {
      if (!reader_running)
	return;
      cond.Signal();
      reader_thread.kill(SIGUSR2);
      lock.Unlock();
      reader_thread.join();
      lock.Lock();
    }

    // public constructors
    static const Pipe& Server(int s);
    static const Pipe& Client(const entity_addr_t& pi);

    //we have two queue_received's to allow local signal delivery
    // via Message * (that doesn't actually point to a Message)
    void queue_received(Message *m, int priority) {
      list<Message *>& queue = in_q[priority];
      lock.Lock();
      queue.push_back(m);
      if ( 1 == queue.size()) {
	if (!queue_items.count(priority)) { //create an item for that priority
	  pair< int, xlist<Pipe *>::item >
	    pair_item(priority, xlist<Pipe *>::item(this));
	  queue_items.insert(pair_item);
	}
	rank->local_endpoint->queue_lock.Lock();
	rank->local_endpoint->
	  queued_pipes[priority].push_back(&queue_items[priority]);
	rank->local_endpoint->queue_lock.Unlock();
      }
      lock.Unlock();
    }
    
    void queue_received(Message *m) {
      m->set_recv_stamp(g_clock.now());
      assert (m->nref.test() == 0);
      queue_received(m, m->get_priority() );
    }

    __u32 get_out_seq() { return out_seq; }

    bool is_queued() { return !out_q.empty() || keepalive; }

    entity_addr_t& get_peer_addr() { return peer_addr; }

    void set_peer_addr(const entity_addr_t& a) {
      if (&peer_addr != &a)  // shut up valgrind
	peer_addr = a;
      connection_state->set_peer_addr(a);
    }
    void set_peer_type(int t) {
      peer_type = t;
      connection_state->set_peer_type(t);
    }

    void register_pipe();
    void unregister_pipe();
    void join() {
      if (writer_thread.is_started()) writer_thread.join();
      if (reader_thread.is_started()) reader_thread.join();
    }
    void stop();

    void send(Message *m) {
      lock.Lock();
      _send(m);
      lock.Unlock();
    }    
    void _send(Message *m) {
      m->get();
      out_q[m->get_priority()].push_back(m);
      cond.Signal();
    }
    void send_keepalive() {
      lock.Lock();
      _send_keepalive();
      lock.Unlock();
    }    
    void _send_keepalive() {
      keepalive = true;
      cond.Signal();
    }
    Message *_get_next_outgoing() {
      Message *m = 0;
      while (!m && !out_q.empty()) {
	map<int, list<Message*> >::reverse_iterator p = out_q.rbegin();
	if (!p->second.empty()) {
	  m = p->second.front();
	  p->second.pop_front();
	}
	if (p->second.empty())
	  out_q.erase(p->first);
      }
      return m;
    }

    void requeue_sent();
    void discard_queue();

    void force_close() {
      if (sd >= 0) ::close(sd);
    }
  };


  // messenger interface
  class Endpoint : public Messenger {
    SimpleMessenger *rank;
    Pipe *local_pipe;
    Mutex lock;
    Cond cond;
    Mutex queue_lock;
    map<int, xlist<Pipe *> > queued_pipes;
    map<int, xlist<Pipe *>::iterator> queued_pipe_iters;
    bool stop;
    int qlen;
    int my_rank;

    class DispatchThread : public Thread {
      Endpoint *m;
    public:
      DispatchThread(Endpoint *_m) : m(_m) {}
      void *entry() {
        m->dispatch_entry();
        return 0;
      }
    } dispatch_thread;
    void dispatch_entry();

    friend class SimpleMessenger;

  public:
    enum { D_CONNECT, D_BAD_REMOTE_RESET, D_BAD_RESET };
    list<Connection*> connect_q;
    list<Connection*> remote_reset_q;
    list<Connection*> reset_q;

    void local_delivery(Message *m, int priority) {
      local_pipe->queue_received(m, priority);
    }

    void local_delivery(Message *m) {
      local_pipe->queue_received(m);
    }

    void queue_connect(Connection *con) {
      lock.Lock();
      connect_q.push_back(con);
      local_delivery((Message*)D_CONNECT, CEPH_MSG_PRIO_HIGHEST);
      cond.Signal();
      lock.Unlock();
    }
    void queue_remote_reset(Connection *con) {
      lock.Lock();
      remote_reset_q.push_back(con);
      local_delivery((Message*)D_BAD_REMOTE_RESET, CEPH_MSG_PRIO_HIGHEST);
      cond.Signal();
      lock.Unlock();
    }
    void queue_reset(Connection *con) {
      lock.Lock();
      reset_q.push_back(con);
      local_delivery((Message*)D_BAD_RESET, CEPH_MSG_PRIO_HIGHEST);
      cond.Signal();
      lock.Unlock();
    }

  public:
    Endpoint(SimpleMessenger *r, entity_name_t name, int rn) : 
      Messenger(name),
      rank(r),
      lock("SimpleMessenger::Endpoint::lock"),
      queue_lock("SimpleMessenger::Endpoint:queue_lock"),
      stop(false),
      qlen(0),
      my_rank(rn),
      dispatch_thread(this) {
      local_pipe = new Pipe(r, Pipe::STATE_OPEN);
    }
    ~Endpoint() { delete local_pipe; }

    void destroy() {
      // join dispatch thread
      if (dispatch_thread.is_started())
	dispatch_thread.join();

      Messenger::destroy();
    }

    void ready();
    bool is_stopped() { return stop; }

    void wait() {
      dispatch_thread.join();
    }
    
    int get_dispatch_queue_len() { return qlen; }

    entity_addr_t get_myaddr();


    int shutdown();
    void suicide();
    void prepare_dest(const entity_inst_t& inst);
    int send_message(Message *m, entity_inst_t dest);
    int forward_message(Message *m, entity_inst_t dest);
    int lazy_send_message(Message *m, entity_inst_t dest);
    int send_keepalive(entity_inst_t dest);

    void mark_down(entity_addr_t a);
    void mark_up(entity_name_t a, entity_addr_t& i);
  };


  // SimpleMessenger stuff
 public:
  Mutex lock;
  Cond  wait_cond;  // for wait()
  bool started;
  bool did_bind;

  // where i listen
  bool need_addr;
  entity_addr_t rank_addr;
  
  // local
  /*unsigned max_local, num_local;
  vector<Endpoint*> local;
  vector<bool>             stopped; */
  Endpoint *local_endpoint;
  bool endpoint_stopped;
  
  // remote
  hash_map<entity_addr_t, Pipe*> rank_pipe;
 
  int my_type;
  Policy default_policy;
  map<int, Policy> policy_map; // entity_name_t::type -> Policy

  set<Pipe*>      pipes;
  list<Pipe*>     pipe_reap_queue;
  
  Mutex global_seq_lock;
  __u32 global_seq;
      
  Pipe *connect_rank(const entity_addr_t& addr, int type);

  const entity_addr_t &get_rank_addr() { return rank_addr; }

  void mark_down(entity_addr_t addr);

  void reaper();

  Policy get_policy(int t) {
    if (policy_map.count(t))
      return policy_map[t];
    else
      return default_policy;
  }

public:
  SimpleMessenger() :
    accepter(this),
    lock("SimpleMessenger::lock"), started(false), did_bind(false), need_addr(true),
    local_endpoint(NULL), my_type(-1),
    global_seq_lock("SimpleMessenger::global_seq_lock"), global_seq(0) { }
  ~SimpleMessenger() { }

  //void set_listen_addr(tcpaddr_t& a);

  int bind(int64_t force_nonce = -1);
  int start(bool nodaemon = false);
  void wait();

  __u32 get_global_seq(__u32 old=0) {
    Mutex::Locker l(global_seq_lock);
    if (old > global_seq)
      global_seq = old;
    return ++global_seq;
  }

  AuthAuthorizer *get_authorizer(int peer_type, bool force_new);
  bool verify_authorizer(Connection *con, int peer_type, int protocol, bufferlist& auth, bufferlist& auth_reply,
			 bool& isvalid);

  Endpoint *register_entity(entity_name_t addr);
  void rename_entity(Endpoint *ms, entity_name_t newaddr);
  void unregister_entity(Endpoint *ms);

  void submit_message(Message *m, const entity_inst_t& addr, bool lazy=false);  
  void prepare_dest(const entity_inst_t& inst);
  void send_keepalive(const entity_inst_t& addr);  

  void learned_addr(entity_addr_t peer_addr_for_me);

  // create a new messenger
  Endpoint *new_entity(entity_name_t addr);

  void set_default_policy(Policy p) {
    default_policy = p;
  }
  void set_policy(int type, Policy p) {
    policy_map[type] = p;
  }
} ;

#endif
