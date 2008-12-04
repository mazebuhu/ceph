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



#include "include/types.h"
#include "common/Clock.h"

#include "msg/Messenger.h"

#include "osd/OSDMap.h"
#include "osdc/Objecter.h"
#include "osdc/Filer.h"

#include "MDSMap.h"

#include "MDS.h"
#include "Server.h"
#include "Locker.h"
#include "MDCache.h"
#include "MDLog.h"
#include "MDBalancer.h"
#include "Migrator.h"

#include "AnchorServer.h"
#include "AnchorClient.h"
#include "SnapServer.h"
#include "SnapClient.h"

#include "InoTable.h"

#include "common/Logger.h"
#include "common/LogType.h"

#include "common/Timer.h"

#include "events/ESession.h"

#include "messages/MMDSMap.h"
#include "messages/MMDSBeacon.h"

#include "messages/MGenericMessage.h"

#include "messages/MOSDMap.h"
#include "messages/MOSDGetMap.h"

#include "messages/MClientRequest.h"
#include "messages/MClientRequestForward.h"

#include "messages/MMDSTableRequest.h"

#include "messages/MMonCommand.h"

#include "config.h"

#define DOUT_SUBSYS mds
#undef dout_prefix
#define dout_prefix *_dout << dbeginl << "mds" << whoami << " "



// cons/des
MDS::MDS(int whoami, Messenger *m, MonMap *mm) : 
  mds_lock("MDS::mds_lock"),
  timer(mds_lock), 
  sessionmap(this) {

  this->whoami = whoami;

  last_tid = 0;

  monmap = mm;
  messenger = m;

  mdsmap = new MDSMap;
  osdmap = new OSDMap;

  objecter = new Objecter(messenger, monmap, osdmap, mds_lock);
  filer = new Filer(objecter);

  mdcache = new MDCache(this);
  mdlog = new MDLog(this);
  balancer = new MDBalancer(this);

  inotable = new InoTable(this);
  snapserver = new SnapServer(this);
  snapclient = new SnapClient(this);
  anchorserver = new AnchorServer(this);
  anchorclient = new AnchorClient(this);

  server = new Server(this);
  locker = new Locker(this, mdcache);

  // clients
  last_client_mdsmap_bcast = 0;
  
  // beacon
  beacon_last_seq = 0;
  beacon_sender = 0;
  beacon_killer = 0;
  laggy = false;

  // tick
  tick_event = 0;

  req_rate = 0;

  want_state = state = MDSMap::STATE_DNE;

  logger = logger2 = 0;

  // i'm ready!
  messenger->set_dispatcher(this);
}

MDS::~MDS() {
  Mutex::Locker lock(mds_lock);

  if (mdcache) { delete mdcache; mdcache = NULL; }
  if (mdlog) { delete mdlog; mdlog = NULL; }
  if (balancer) { delete balancer; balancer = NULL; }
  if (inotable) { delete inotable; inotable = NULL; }
  if (anchorserver) { delete anchorserver; anchorserver = NULL; }
  if (snapserver) { delete snapserver; snapserver = NULL; }
  if (snapclient) { delete snapclient; snapclient = NULL; }
  if (anchorclient) { delete anchorclient; anchorclient = NULL; }
  if (osdmap) { delete osdmap; osdmap = 0; }
  if (mdsmap) { delete mdsmap; mdsmap = 0; }

  if (server) { delete server; server = 0; }
  if (locker) { delete locker; locker = 0; }

  if (filer) { delete filer; filer = 0; }
  if (objecter) { delete objecter; objecter = 0; }

  if (logger) { delete logger; logger = 0; }
  if (logger2) { delete logger2; logger2 = 0; }
  
  if (messenger)
    messenger->destroy();
}


void MDS::reopen_logger(utime_t start)
{
  static LogType mds_logtype, mds_cache_logtype;
  static bool didit = false;
  if (!didit) {
    didit = true;
    
    //mds_logtype.add_inc("req");
    mds_logtype.add_inc("reply");
    mds_logtype.add_inc("fw");
    
    mds_logtype.add_inc("dir_f");
    mds_logtype.add_inc("dir_c");
    //mds_logtype.add_inc("mkdir");

    /*
    mds_logtype.add_inc("newin"); // new inodes (pre)loaded
    mds_logtype.add_inc("newt");  // inodes first touched/used
    mds_logtype.add_inc("outt");  // trimmed touched
    mds_logtype.add_inc("outut"); // trimmed untouched (wasted effort)
    mds_logtype.add_avg("oututl"); // avg trim latency for untouched

    mds_logtype.add_inc("dirt1");
    mds_logtype.add_inc("dirt2");
    mds_logtype.add_inc("dirt3");
    mds_logtype.add_inc("dirt4");
    mds_logtype.add_inc("dirt5");
    */

    mds_logtype.add_set("c");
    mds_logtype.add_set("ctop");
    mds_logtype.add_set("cbot");
    mds_logtype.add_set("cptail");  
    mds_logtype.add_set("cpin");
    mds_logtype.add_inc("cex");
    mds_logtype.add_inc("dis");

    mds_logtype.add_inc("t"); 
    mds_logtype.add_inc("thit");
    mds_logtype.add_inc("tfw");
    mds_logtype.add_inc("tdis");
    mds_logtype.add_inc("tdirf");
    mds_logtype.add_inc("trino");
    mds_logtype.add_inc("tlock");
    
    mds_logtype.add_set("l");
    mds_logtype.add_set("q");
    mds_logtype.add_set("popanyd");
    mds_logtype.add_set("popnest");
    
    mds_logtype.add_set("buf");
    
    mds_logtype.add_set("sm");
    mds_logtype.add_inc("ex");
    mds_logtype.add_inc("iex");
    mds_logtype.add_inc("im");
    mds_logtype.add_inc("iim");
    /*
    mds_logtype.add_inc("imex");  
    mds_logtype.add_set("nex");
    mds_logtype.add_set("nim");
    */

    mds_logtype.add_avg("replyl");

  }
 
  if (whoami < 0) return;

  // flush+close old log
  if (logger) delete logger;
  if (logger2) delete logger2;

  // log
  char name[80];
  sprintf(name, "mds%d", whoami);

  bool append = mdsmap->get_inc(whoami) > 1;

  logger = new Logger(name, (LogType*)&mds_logtype, append);
  logger->set_start(start);

  char n[80];
  sprintf(n, "mds%d.cache", whoami);
  logger2 = new Logger(n, (LogType*)&mds_cache_logtype, append);
  logger2->set_start(start);

  mdlog->reopen_logger(start, append);
  server->reopen_logger(start, append);
}




MDSTableClient *MDS::get_table_client(int t)
{
  switch (t) {
  case TABLE_ANCHOR: return anchorclient;
  case TABLE_SNAP: return snapclient;
  default: assert(0);
  }
}

MDSTableServer *MDS::get_table_server(int t)
{
  switch (t) {
  case TABLE_ANCHOR: return anchorserver;
  case TABLE_SNAP: return snapserver;
  default: assert(0);
  }
}










void MDS::send_message_mds(Message *m, int mds)
{
  // send mdsmap first?
  if (peer_mdsmap_epoch[mds] < mdsmap->get_epoch()) {
    messenger->send_message(new MMDSMap(monmap->fsid, mdsmap), 
			    mdsmap->get_inst(mds));
    peer_mdsmap_epoch[mds] = mdsmap->get_epoch();
  }

  // send message
  messenger->send_message(m, mdsmap->get_inst(mds));
}

void MDS::forward_message_mds(Message *m, int mds)
{
  // client request?
  if (m->get_type() == CEPH_MSG_CLIENT_REQUEST &&
      ((MClientRequest*)m)->get_orig_source().is_client()) {
    MClientRequest *creq = (MClientRequest*)m;
    creq->inc_num_fwd();    // inc forward counter

    /*
     * don't actually forward if non-idempotent!
     * client has to do it.  although the MDS will ignore duplicate requests,
     * the affected metadata may migrate, in which case the new authority
     * won't have the metareq_id in the completed request map.
     */
    bool client_must_resend = !creq->can_forward();

    // tell the client where it should go
    messenger->send_message(new MClientRequestForward(creq->get_tid(), mds, creq->get_num_fwd(),
						      client_must_resend),
			    creq->get_orig_source_inst());
    
    if (client_must_resend) {
      delete m;
      return; 
    }
  }

  // send mdsmap first?
  if (peer_mdsmap_epoch[mds] < mdsmap->get_epoch()) {
    messenger->send_message(new MMDSMap(monmap->fsid, mdsmap), 
			    mdsmap->get_inst(mds));
    peer_mdsmap_epoch[mds] = mdsmap->get_epoch();
  }

  messenger->forward_message(m, mdsmap->get_inst(mds));
}



void MDS::send_message_client(Message *m, int client)
{
  if (sessionmap.have_session(entity_name_t::CLIENT(client))) {
    version_t seq = sessionmap.inc_push_seq(client);
    dout(10) << "send_message_client client" << client << " seq " << seq << " " << *m << dendl;
    messenger->send_message(m, sessionmap.get_session(entity_name_t::CLIENT(client))->inst);
  } else {
    dout(10) << "send_message_client no session for client" << client << " " << *m << dendl;
  }
}

void MDS::send_message_client(Message *m, entity_inst_t clientinst)
{
  version_t seq = sessionmap.inc_push_seq(clientinst.name.num());
  dout(10) << "send_message_client " << clientinst.name << " seq " << seq << " " << *m << dendl;
  messenger->send_message(m, clientinst);
}



int MDS::init(bool standby)
{
  mds_lock.Lock();

  // starting beacon.  this will induce an MDSMap from the monitor
  want_state = MDSMap::STATE_BOOT;
  want_rank = whoami;
  beacon_start();
  whoami = -1;
  messenger->reset_myname(entity_name_t::MDS(whoami));

  objecter->init();
   
  // schedule tick
  reset_tick();

  mds_lock.Unlock();
  return 0;
}

void MDS::reset_tick()
{
  // cancel old
  if (tick_event) timer.cancel_event(tick_event);

  // schedule
  tick_event = new C_MDS_Tick(this);
  timer.add_event_after(g_conf.mds_tick_interval, tick_event);
}

void MDS::tick()
{
  tick_event = 0;

  // reschedule
  reset_tick();

  if (laggy)
    return;

  // log
  mds_load_t load = balancer->get_load();
  
  if (logger) {
    req_rate = logger->get("req");
    
    logger->fset("l", (int)load.mds_load());
    logger->set("q", messenger->get_dispatch_queue_len());
    logger->set("buf", buffer_total_alloc.test());
    logger->set("sm", mdcache->num_subtrees());

    mdcache->log_stat(logger);
  }

  // ...
  if (is_active() || is_stopping()) {
    locker->scatter_tick();
    server->find_idle_sessions();
  }
  
  if (is_reconnect())
    server->reconnect_tick();
  
  if (is_active()) {
    balancer->tick();
    if (snapserver)
      snapserver->check_osd_map(false);
  }
}




// -----------------------
// beacons

void MDS::beacon_start()
{
  beacon_send();         // send first beacon
  
  //reset_beacon_killer(); // schedule killer
}
  


void MDS::beacon_send()
{
  ++beacon_last_seq;
  dout(10) << "beacon_send " << MDSMap::get_state_name(want_state)
	   << " seq " << beacon_last_seq
	   << " (currently " << MDSMap::get_state_name(state) << ")"
	   << dendl;

  // pick new random mon if we have any outstanding beacons...
  int mon = monmap->pick_mon(beacon_seq_stamp.size());

  beacon_seq_stamp[beacon_last_seq] = g_clock.now();
  
  messenger->send_message(new MMDSBeacon(monmap->fsid, mdsmap->get_epoch(), 
					 want_state, beacon_last_seq, want_rank),
			  monmap->get_inst(mon));

  // schedule next sender
  if (beacon_sender) timer.cancel_event(beacon_sender);
  beacon_sender = new C_MDS_BeaconSender(this);
  timer.add_event_after(g_conf.mds_beacon_interval, beacon_sender);
}

void MDS::handle_mds_beacon(MMDSBeacon *m)
{
  dout(10) << "handle_mds_beacon " << MDSMap::get_state_name(m->get_state())
	   << " seq " << m->get_seq() << dendl;
  version_t seq = m->get_seq();

  // make note of which mon 
  monmap->last_mon = m->get_source().num();

  // update lab
  if (beacon_seq_stamp.count(seq)) {
    assert(beacon_seq_stamp[seq] > beacon_last_acked_stamp);
    beacon_last_acked_stamp = beacon_seq_stamp[seq];
    
    // clean up seq_stamp map
    while (!beacon_seq_stamp.empty() &&
	   beacon_seq_stamp.begin()->first <= seq)
      beacon_seq_stamp.erase(beacon_seq_stamp.begin());

    if (laggy &&
	g_clock.now() - beacon_last_acked_stamp < g_conf.mds_beacon_grace) {
      dout(1) << " clearing laggy flag" << dendl;
      laggy = false;
      queue_waiters(waiting_for_nolaggy);
    }
    
    reset_beacon_killer();
  }

  delete m;
}

void MDS::reset_beacon_killer()
{
  utime_t when = beacon_last_acked_stamp;
  when += g_conf.mds_beacon_grace;
  
  dout(25) << "reset_beacon_killer last_acked_stamp at " << beacon_last_acked_stamp
	   << ", will die at " << when << dendl;
  
  if (beacon_killer) timer.cancel_event(beacon_killer);

  beacon_killer = new C_MDS_BeaconKiller(this, beacon_last_acked_stamp);
  timer.add_event_at(when, beacon_killer);
}

void MDS::beacon_kill(utime_t lab)
{
  if (lab == beacon_last_acked_stamp) {
    dout(0) << "beacon_kill last_acked_stamp " << lab 
	    << ", setting laggy flag."
	    << dendl;
    laggy = true;
    //suicide();
  } else {
    dout(20) << "beacon_kill last_acked_stamp " << beacon_last_acked_stamp 
	     << " != my " << lab 
	     << ", doing nothing."
	     << dendl;
  }
}



void MDS::handle_mds_map(MMDSMap *m)
{
  version_t epoch = m->get_epoch();
  dout(5) << "handle_mds_map epoch " << epoch << " from " << m->get_source() << dendl;

  // note source's map version
  if (m->get_source().is_mds() && 
      peer_mdsmap_epoch[m->get_source().num()] < epoch) {
    dout(15) << " peer " << m->get_source()
	     << " has mdsmap epoch >= " << epoch
	     << dendl;
    peer_mdsmap_epoch[m->get_source().num()] = epoch;
  }

  // is it new?
  if (epoch <= mdsmap->get_epoch()) {
    dout(5) << " old map epoch " << epoch << " <= " << mdsmap->get_epoch() 
	    << ", discarding" << dendl;
    delete m;
    return;
  }

  // keep old map, for a moment
  MDSMap *oldmap = mdsmap;
  int oldwhoami = whoami;
  int oldstate = state;

  // decode and process
  mdsmap = new MDSMap;
  mdsmap->decode(m->get_encoded());
  
  // see who i am
  whoami = mdsmap->get_addr_rank(messenger->get_myaddr());
  if (whoami < 0) {
    if (mdsmap->is_standby(messenger->get_myaddr())) {
      if (state != MDSMap::STATE_STANDBY) {
	want_state = state = MDSMap::STATE_STANDBY;
	dout(1) << "handle_mds_map standby" << dendl;
      }
      goto out;
    }
    dout(1) << "handle_mds_map i (" << messenger->get_myaddr() 
	    << ") am not in the mdsmap, killing myself" << dendl;
    suicide();
    goto out;
  }

  // open logger?
  //  note that fakesyn/newsyn starts knowing who they are
  if (whoami >= 0 &&
      mdsmap->is_up(whoami) &&
      (oldwhoami != whoami || !logger)) {
    _dout_create_courtesy_output_symlink("mds", whoami);
    reopen_logger(mdsmap->get_created());   // adopt mds cluster timeline
  }
  
  if (oldwhoami != whoami) {
    // update messenger.
    dout(1) << "handle_mds_map i am now mds" << whoami
	    << " incarnation " << mdsmap->get_inc(whoami)
	    << dendl;
    messenger->reset_myname(entity_name_t::MDS(whoami));

    // do i need an osdmap?
    if (oldwhoami < 0) {
      // we need an osdmap too.
      int mon = monmap->pick_mon();
      messenger->send_message(new MOSDGetMap(monmap->fsid, 0),
			      monmap->get_inst(mon));
    }
  }

  // tell objecter my incarnation
  if (objecter->get_client_incarnation() < 0 &&
      mdsmap->have_inst(whoami)) {
    assert(mdsmap->get_inc(whoami) > 0);
    objecter->set_client_incarnation(mdsmap->get_inc(whoami));
  }
  // and inc_lock
  objecter->set_inc_lock(mdsmap->get_last_failure());

  // for debug
  if (g_conf.mds_dump_cache_on_map)
    mdcache->dump_cache();

  // update my state
  state = mdsmap->get_state(whoami);
  
  // did it change?
  if (oldstate != state) {
    dout(1) << "handle_mds_map state change "
	    << mdsmap->get_state_name(oldstate) << " --> "
	    << mdsmap->get_state_name(state) << dendl;
    want_state = state;

    // now active?
    if (is_active()) {
      // did i just recover?
      if (oldstate == MDSMap::STATE_REJOIN ||
	  oldstate == MDSMap::STATE_RECONNECT) 
	recovery_done();
      finish_contexts(waiting_for_active);  // kick waiters
    } else if (is_replay()) {
      replay_start();
    } else if (is_resolve()) {
      resolve_start();
    } else if (is_reconnect()) {
      reconnect_start();
    } else if (is_creating()) {
      boot_create();
    } else if (is_starting()) {
      boot_start();
    } else if (is_stopping()) {
      assert(oldstate == MDSMap::STATE_ACTIVE);
      stopping_start();
    } else if (is_stopped()) {
      assert(oldstate == MDSMap::STATE_STOPPING);
      suicide();
      return;
    }
  }

  
  // RESOLVE
  // is someone else newly resolving?
  if (is_resolve() || is_rejoin() || is_active() || is_stopping()) {
    set<int> oldresolve, resolve;
    oldmap->get_mds_set(oldresolve, MDSMap::STATE_RESOLVE);
    mdsmap->get_mds_set(resolve, MDSMap::STATE_RESOLVE);
    if (oldresolve != resolve) {
      dout(10) << "resolve set is " << resolve << ", was " << oldresolve << dendl;
      for (set<int>::iterator p = resolve.begin(); p != resolve.end(); ++p) 
	if (*p != whoami &&
	    oldresolve.count(*p) == 0)
	  mdcache->send_resolve(*p);  // now or later.
    }
  }
  
  // REJOIN
  // is everybody finally rejoining?
  if (is_rejoin() || is_active() || is_stopping()) {
    // did we start?
    if (!oldmap->is_rejoining() && mdsmap->is_rejoining())
      rejoin_joint_start();

    // did we finish?
    if (g_conf.mds_dump_cache_after_rejoin &&
	oldmap->is_rejoining() && !mdsmap->is_rejoining()) 
      mdcache->dump_cache();      // for DEBUG only
  }
  if (oldmap->is_degraded() && !mdsmap->is_degraded() && state >= MDSMap::STATE_ACTIVE)
    dout(1) << "cluster recovered." << dendl;
  
  // did someone go active?
  if (is_active() || is_stopping()) {
    set<int> oldactive, active;
    oldmap->get_mds_set(oldactive, MDSMap::STATE_ACTIVE);
    mdsmap->get_mds_set(active, MDSMap::STATE_ACTIVE);
    for (set<int>::iterator p = active.begin(); p != active.end(); ++p) 
      if (*p != whoami &&            // not me
	  oldactive.count(*p) == 0)  // newly so?
	handle_mds_recovery(*p);
  }

  // did someone fail?
  if (true) {
    // new failed?
    set<int> oldfailed, failed;
    oldmap->get_mds_set(oldfailed, MDSMap::STATE_FAILED);
    mdsmap->get_mds_set(failed, MDSMap::STATE_FAILED);
    for (set<int>::iterator p = failed.begin(); p != failed.end(); ++p)
      if (oldfailed.count(*p) == 0)
	mdcache->handle_mds_failure(*p);
    
    // or down then up?
    //  did their addr/inst change?
    set<int> up;
    mdsmap->get_up_mds_set(up);
    for (set<int>::iterator p = up.begin(); p != up.end(); ++p) 
      if (oldmap->have_inst(*p) &&
	  oldmap->get_inst(*p) != mdsmap->get_inst(*p))
	mdcache->handle_mds_failure(*p);
  }
  if (is_active() || is_stopping()) {
    // did anyone stop?
    set<int> oldstopped, stopped;
    oldmap->get_mds_set(oldstopped, MDSMap::STATE_STOPPED);
    mdsmap->get_mds_set(stopped, MDSMap::STATE_STOPPED);
    for (set<int>::iterator p = stopped.begin(); p != stopped.end(); ++p) 
      if (oldstopped.count(*p) == 0)      // newly so?
	mdcache->migrator->handle_mds_failure_or_stop(*p);
  }

 out:
  delete m;
  delete oldmap;
}

void MDS::bcast_mds_map()
{
  dout(7) << "bcast_mds_map " << mdsmap->get_epoch() << dendl;

  // share the map with mounted clients
  set<Session*> clients;
  sessionmap.get_client_session_set(clients);
  for (set<Session*>::const_iterator p = clients.begin();
       p != clients.end();
       ++p) 
    messenger->send_message(new MMDSMap(monmap->fsid, mdsmap), (*p)->inst);
  last_client_mdsmap_bcast = mdsmap->get_epoch();
}


void MDS::request_state(int s)
{
  dout(3) << "request_state " << MDSMap::get_state_name(s) << dendl;
  want_state = s;
  beacon_send();
}


class C_MDS_CreateFinish : public Context {
  MDS *mds;
public:
  C_MDS_CreateFinish(MDS *m) : mds(m) {}
  void finish(int r) { mds->creating_done(); }
};

void MDS::boot_create()
{
  dout(3) << "boot_create" << dendl;

  C_Gather *fin = new C_Gather(new C_MDS_CreateFinish(this));

  CDir *rootdir = 0;
  if (whoami == 0) {
    dout(3) << "boot_create since i am also mds0, creating root inode and dir" << dendl;

    // create root inode.
    mdcache->open_root(0);
    CInode *root = mdcache->get_root();
    assert(root);
    
    // force empty root dir
    rootdir = root->get_dirfrag(frag_t());
    rootdir->mark_complete();
  }

  // create my stray dir
  CDir *straydir;
  {
    dout(10) << "boot_create creating local stray dir" << dendl;
    mdcache->open_local_stray();
    CInode *stray = mdcache->get_stray();
    straydir = stray->get_dirfrag(frag_t());
    straydir->mark_complete();
  }

  // start with a fresh journal
  dout(10) << "boot_create creating fresh journal" << dendl;
  mdlog->create(fin->new_sub());
  
  // write our first subtreemap
  mdlog->start_new_segment(fin->new_sub());

  // dirty, commit (root and) stray dir(s)
  if (whoami == 0) {
    rootdir->mark_dirty(rootdir->pre_dirty(), mdlog->get_current_segment());
    rootdir->commit(0, fin->new_sub());
  }
  straydir->mark_dirty(straydir->pre_dirty(), mdlog->get_current_segment());
  straydir->commit(0, fin->new_sub());
 
  // fixme: fake out inotable (reset, pretend loaded)
  dout(10) << "boot_create creating fresh inotable table" << dendl;
  inotable->reset();
  inotable->save(fin->new_sub());

  // write empty sessionmap
  sessionmap.save(fin->new_sub());
  
  // initialize tables
  if (mdsmap->get_tableserver() == whoami) {
    dout(10) << "boot_create creating fresh anchortable" << dendl;
    anchorserver->reset();
    anchorserver->save(fin->new_sub());

    dout(10) << "boot_create creating fresh snaptable" << dendl;
    snapserver->reset();
    snapserver->save(fin->new_sub());
  }
}

void MDS::creating_done()
{
  dout(1)<< "creating_done" << dendl;
  request_state(MDSMap::STATE_ACTIVE);
}


class C_MDS_BootStart : public Context {
  MDS *mds;
  int nextstep;
public:
  C_MDS_BootStart(MDS *m, int n) : mds(m), nextstep(n) {}
  void finish(int r) { mds->boot_start(nextstep, r); }
};

void MDS::boot_start(int step, int r)
{
  if (r < 0) {
    dout(0) << "boot_start encountered an error, failing" << dendl;
    suicide();
    return;
  }

  switch (step) {
  case 0:
    step = 1;  // fall-thru.

  case 1:
    {
      C_Gather *gather = new C_Gather(new C_MDS_BootStart(this, 2));
      dout(2) << "boot_start " << step << ": opening inotable" << dendl;
      inotable->load(gather->new_sub());

      dout(2) << "boot_start " << step << ": opening sessionmap" << dendl;
      sessionmap.load(gather->new_sub());

      if (mdsmap->get_tableserver() == whoami) {
	dout(2) << "boot_start " << step << ": opening anchor table" << dendl;
	anchorserver->load(gather->new_sub());

	dout(2) << "boot_start " << step << ": opening snap table" << dendl;	
	snapserver->load(gather->new_sub());
      }
      
      dout(2) << "boot_start " << step << ": opening mds log" << dendl;
      mdlog->open(gather->new_sub());
    }
    break;

  case 2:
    if (is_replay()) {
      dout(2) << "boot_start " << step << ": replaying mds log" << dendl;
      mdlog->replay(new C_MDS_BootStart(this, 3));
      break;
    } else {
      dout(2) << "boot_start " << step << ": positioning at end of old mds log" << dendl;
      mdlog->append();
      step++;
    }

  case 3:
    if (is_replay()) {
      replay_done();
      break;
    }

    // starting only
    assert(is_starting());
    if (mdsmap->get_root() == whoami) {
      dout(2) << "boot_start " << step << ": opening root directory" << dendl;
      mdcache->open_root(new C_MDS_BootStart(this, 4));
      break;
    }
    step++;
    
  case 4:
    dout(2) << "boot_start " << step << ": opening local stray directory" << dendl;
    mdcache->open_local_stray();

    starting_done();
    break;
  }
}

void MDS::starting_done()
{
  dout(3) << "starting_done" << dendl;
  assert(is_starting());
  request_state(MDSMap::STATE_ACTIVE);

  // start new segment
  mdlog->start_new_segment(0);
}


void MDS::replay_start()
{
  dout(1) << "replay_start" << dendl;

  // initialize gather sets
  set<int> rs;
  mdsmap->get_recovery_mds_set(rs);
  rs.erase(whoami);
  dout(1) << "now replay.  my recovery peers are " << rs << dendl;
  mdcache->set_recovery_set(rs);

  // start?
  //if (osdmap->get_epoch() > 0 &&
  //mdsmap->get_epoch() > 0)
  boot_start();
}

void MDS::replay_done()
{
  dout(1) << "replay_done in=" << mdsmap->get_num_in_mds()
	  << " failed=" << mdsmap->get_num_mds(MDSMap::STATE_FAILED)
	  << dendl;

  if (mdsmap->get_num_in_mds() == 1 &&
      mdsmap->get_num_mds(MDSMap::STATE_FAILED) == 0) { // just me!
    dout(2) << "i am alone, moving to state reconnect" << dendl;      
    request_state(MDSMap::STATE_RECONNECT);
  } else {
    dout(2) << "i am not alone, moving to state resolve" << dendl;
    request_state(MDSMap::STATE_RESOLVE);
  }

  // start new segment
  mdlog->start_new_segment(0);
}


void MDS::resolve_start()
{
  dout(1) << "resolve_start" << dendl;

  set<int> who;
  mdsmap->get_mds_set(who, MDSMap::STATE_RESOLVE);
  mdsmap->get_mds_set(who, MDSMap::STATE_REJOIN);
  mdsmap->get_mds_set(who, MDSMap::STATE_ACTIVE);
  mdsmap->get_mds_set(who, MDSMap::STATE_STOPPING);
  for (set<int>::iterator p = who.begin(); p != who.end(); ++p) {
    if (*p == whoami) continue;
    mdcache->send_resolve(*p);  // now.
  }
}
void MDS::resolve_done()
{
  dout(1) << "resolve_done" << dendl;
  request_state(MDSMap::STATE_RECONNECT);
}

void MDS::reconnect_start()
{
  dout(1) << "reconnect_start" << dendl;
  server->reconnect_clients();
}
void MDS::reconnect_done()
{
  dout(1) << "reconnect_done" << dendl;
  request_state(MDSMap::STATE_REJOIN);    // move to rejoin state

  mdcache->reconnect_clean_open_file_lists();

  /*
  if (mdsmap->get_num_in_mds() == 1 &&
      mdsmap->get_num_mds(MDSMap::STATE_FAILED) == 0) { // just me!

    // finish processing caps (normally, this happens during rejoin, but we're skipping that...)
    mdcache->rejoin_gather_finish();

    request_state(MDSMap::STATE_ACTIVE);    // go active
  } else {
    request_state(MDSMap::STATE_REJOIN);    // move to rejoin state
  }
  */
}

void MDS::rejoin_joint_start()
{
  dout(1) << "rejoin_joint_start" << dendl;
  mdcache->rejoin_send_rejoins();
}
void MDS::rejoin_done()
{
  dout(1) << "rejoin_done" << dendl;
  mdcache->show_subtrees();
  mdcache->show_cache();
  request_state(MDSMap::STATE_ACTIVE);
}


void MDS::recovery_done()
{
  dout(1) << "recovery_done -- successful recovery!" << dendl;
  assert(is_active());
  
  // kick anchortable (resent AGREEs)
  if (mdsmap->get_tableserver() == whoami) {
    anchorserver->finish_recovery();
    snapserver->finish_recovery();
  }
  
  // kick anchorclient (resent COMMITs)
  anchorclient->finish_recovery();
  snapclient->finish_recovery();
  
  mdcache->start_recovered_purges();
  mdcache->do_file_recover();
  
  // tell connected clients
  bcast_mds_map();  

  queue_waiters(waiting_for_active);
}

void MDS::handle_mds_recovery(int who) 
{
  dout(5) << "handle_mds_recovery mds" << who << dendl;
  
  mdcache->handle_mds_recovery(who);

  if (anchorserver) {
    anchorserver->handle_mds_recovery(who);
    snapserver->handle_mds_recovery(who);
  }
  anchorclient->handle_mds_recovery(who);
  snapclient->handle_mds_recovery(who);
  
  queue_waiters(waiting_for_active_peer[who]);
  waiting_for_active_peer.erase(who);
}

void MDS::stopping_start()
{
  dout(2) << "stopping_start" << dendl;

  // start cache shutdown
  mdcache->shutdown_start();
  
  // terminate client sessions
  server->terminate_sessions();
}

void MDS::stopping_done()
{
  dout(2) << "stopping_done" << dendl;

  // tell monitor we shut down cleanly.
  request_state(MDSMap::STATE_STOPPED);
}

  

void MDS::suicide()
{
  dout(1) << "suicide" << dendl;

  // stop timers
  if (beacon_killer) {
    timer.cancel_event(beacon_killer);
    beacon_killer = 0;
  }
  if (beacon_sender) {
    timer.cancel_event(beacon_sender);
    beacon_sender = 0;
  }
  if (tick_event) {
    timer.cancel_event(tick_event);
    tick_event = 0;
  }
  timer.cancel_all();
  //timer.join();  // this will deadlock from beacon_kill -> suicide
  
  // shut down cache
  mdcache->shutdown();

  objecter->shutdown();
  
  // shut down messenger
  messenger->shutdown();
}





bool MDS::dispatch_impl(Message *m)
{
  bool ret;

  // verify protocol version
  if (m->get_orig_source().is_mds() &&
      m->get_header().mds_protocol != CEPH_MDS_PROTOCOL) {
    dout(0) << "mds protocol v " << (int)m->get_header().mds_protocol << " != my " << CEPH_MDS_PROTOCOL
	    << " from " << m->get_orig_source_inst() << " " << *m << dendl;
    delete m;
    return true;
  }

  if (m->get_header().mdsc_protocol != CEPH_MDSC_PROTOCOL) {
    dout(0) << "mdsc protocol v " << (int)m->get_header().mdsc_protocol << " != my " << CEPH_MDSC_PROTOCOL
	    << " from " << m->get_orig_source_inst() << " " << *m << dendl;
    delete m;
    return true;
  }
  if (m->get_orig_source().is_mon() &&
      m->get_header().monc_protocol != CEPH_MONC_PROTOCOL) {
    dout(0) << "monc protocol v " << (int)m->get_header().monc_protocol << " != my " << CEPH_MONC_PROTOCOL
	    << " from " << m->get_orig_source_inst() << " " << *m << dendl;
    delete m;
    return true;
  }
  if (m->get_orig_source().is_osd() &&
      m->get_header().osdc_protocol != CEPH_OSDC_PROTOCOL) {
    dout(0) << "osdc protocol v " << (int)m->get_header().osdc_protocol << " != my " << CEPH_OSDC_PROTOCOL
	    << " from " << m->get_orig_source_inst() << " " << *m << dendl;
    delete m;
    return true;
  }

  mds_lock.Lock();
  ret = _dispatch(m);
  mds_lock.Unlock();

  return ret;
}



bool MDS::_dispatch(Message *m)
{
  // from bad mds?
  if (m->get_source().is_mds()) {
    int from = m->get_source().num();
    if (!mdsmap->have_inst(from) ||
	mdsmap->get_inst(from) != m->get_source_inst() ||
	mdsmap->is_down(from)) {
      // bogus mds?
      if (m->get_type() == CEPH_MSG_MDS_MAP) {
	dout(5) << "got " << *m << " from old/bad/imposter mds " << m->get_source()
		<< ", but it's an mdsmap, looking at it" << dendl;
      } else if (m->get_type() == MSG_MDS_CACHEEXPIRE &&
		 mdsmap->get_inst(from) == m->get_source_inst()) {
	dout(5) << "got " << *m << " from down mds " << m->get_source()
		<< ", but it's a cache_expire, looking at it" << dendl;
      } else {
	dout(5) << "got " << *m << " from down/old/bad/imposter mds " << m->get_source()
		<< ", dropping" << dendl;
	delete m;
	return true;
      }
    }
  }

  switch (m->get_type()) {
    // MDS
  case CEPH_MSG_MDS_MAP:
    handle_mds_map((MMDSMap*)m);
    break;
  case MSG_MDS_BEACON:
    handle_mds_beacon((MMDSBeacon*)m);
    break;
    
    // misc
  case MSG_MON_COMMAND:
    parse_config_option_string(((MMonCommand*)m)->cmd[0]);
    delete m;
    break;    
    
  default:
    
    if (laggy) {
      dout(10) << "laggy, deferring " << *m << dendl;
      waiting_for_nolaggy.push_back(new C_MDS_RetryMessage(this, m));
    } else {
      int port = m->get_type() & 0xff00;
      switch (port) {
      case MDS_PORT_CACHE:
	mdcache->dispatch(m);
	break;
	
      case MDS_PORT_LOCKER:
	locker->dispatch(m);
	break;
	
      case MDS_PORT_MIGRATOR:
	mdcache->migrator->dispatch(m);
	break;
	
      default:
	switch (m->get_type()) {
	  // SERVER
	case CEPH_MSG_CLIENT_SESSION:
	case CEPH_MSG_CLIENT_REQUEST:
	case CEPH_MSG_CLIENT_RECONNECT:
	case MSG_MDS_SLAVE_REQUEST:
	  server->dispatch(m);
	  break;
	  
	case MSG_MDS_HEARTBEAT:
	  balancer->proc_message(m);
	  break;
	  
	case MSG_MDS_TABLE_REQUEST:
	  {
	    MMDSTableRequest *req = (MMDSTableRequest*)m;
	    if (req->op < 0) {
	      MDSTableClient *client = get_table_client(req->table);
	      client->handle_request(req);
	    } else {
	      MDSTableServer *server = get_table_server(req->table);
	      server->handle_request(req);
	    }
	  }
	  break;
	  
	  // OSD
	case CEPH_MSG_OSD_OPREPLY:
	  objecter->handle_osd_op_reply((class MOSDOpReply*)m);
	  break;
	case CEPH_MSG_OSD_MAP:
	  objecter->handle_osd_map((MOSDMap*)m);
	  if (is_active() && snapserver)
	    snapserver->check_osd_map(true);
	  break;
	  
	default:
	  return false;
	}
      }
      
    }
  }


  if (laggy)
    return true;


  // finish any triggered contexts
  if (finished_queue.size()) {
    dout(7) << "mds has " << finished_queue.size() << " queued contexts" << dendl;
    dout(10) << finished_queue << dendl;
    list<Context*> ls;
    ls.splice(ls.begin(), finished_queue);
    assert(finished_queue.empty());
    finish_contexts(ls);
  }


  // HACK FOR NOW
  if (is_active() || is_stopping()) {
    // flush log to disk after every op.  for now.
    //mdlog->flush();
    mdlog->trim();

    // trim cache
    mdcache->trim();
    mdcache->trim_client_leases();
  }

  
  // hack: thrash exports
  static utime_t start;
  utime_t now = g_clock.now();
  if (start == utime_t()) 
    start = now;
  double el = now - start;
  if (el > 30.0 &&
	   el < 60.0)
  for (int i=0; i<g_conf.mds_thrash_exports; i++) {
    set<int> s;
    if (!is_active()) break;
    mdsmap->get_mds_set(s, MDSMap::STATE_ACTIVE);
    if (s.size() < 2 || mdcache->get_num_inodes() < 10) 
      break;  // need peers for this to work.

    dout(7) << "mds thrashing exports pass " << (i+1) << "/" << g_conf.mds_thrash_exports << dendl;
    
    // pick a random dir inode
    CInode *in = mdcache->hack_pick_random_inode();

    list<CDir*> ls;
    in->get_dirfrags(ls);
    if (ls.empty()) continue;                // must be an open dir.
    CDir *dir = ls.front();
    if (!dir->get_parent_dir()) continue;    // must be linked.
    if (!dir->is_auth()) continue;           // must be auth.

    int dest;
    do {
      int k = rand() % s.size();
      set<int>::iterator p = s.begin();
      while (k--) p++;
      dest = *p;
    } while (dest == whoami);
    mdcache->migrator->export_dir_nicely(dir,dest);
  }
  // hack: thrash exports
  for (int i=0; i<g_conf.mds_thrash_fragments; i++) {
    if (!is_active()) break;
    dout(7) << "mds thrashing fragments pass " << (i+1) << "/" << g_conf.mds_thrash_fragments << dendl;
    
    // pick a random dir inode
    CInode *in = mdcache->hack_pick_random_inode();

    list<CDir*> ls;
    in->get_dirfrags(ls);
    if (ls.empty()) continue;                // must be an open dir.
    CDir *dir = ls.front();
    if (!dir->get_parent_dir()) continue;    // must be linked.
    if (!dir->is_auth()) continue;           // must be auth.
    mdcache->split_dir(dir, 1);// + (rand() % 3));
  }

  // hack: force hash root?
  /*
  if (false &&
      mdcache->get_root() &&
      mdcache->get_root()->dir &&
      !(mdcache->get_root()->dir->is_hashed() || 
        mdcache->get_root()->dir->is_hashing())) {
    dout(0) << "hashing root" << dendl;
    mdcache->migrator->hash_dir(mdcache->get_root()->dir);
  }
  */



  // shut down?
  if (is_stopping()) {
    if (mdcache->shutdown_pass()) {
      dout(7) << "shutdown_pass=true, finished w/ shutdown, moving to down:stopped" << dendl;
      stopping_done();
    }
  }
  return true;
}




void MDS::ms_handle_failure(Message *m, const entity_inst_t& inst) 
{
  mds_lock.Lock();
  dout(0) << "ms_handle_failure to " << inst << " on " << *m << dendl;
  mds_lock.Unlock();
}

void MDS::ms_handle_reset(const entity_addr_t& addr, entity_name_t last) 
{
  dout(0) << "ms_handle_reset on " << addr << dendl;
}


void MDS::ms_handle_remote_reset(const entity_addr_t& addr, entity_name_t last) 
{
  dout(0) << "ms_handle_remote_reset on " << addr << dendl;
  objecter->ms_handle_remote_reset(addr, last);
}
