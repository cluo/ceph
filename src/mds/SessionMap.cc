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

#include "MDS.h"
#include "SessionMap.h"
#include "osdc/Filer.h"

#include "config.h"

#define DOUT_SUBSYS mds
#undef dout_prefix
#define dout_prefix *_dout << dbeginl << "mds" << mds->get_nodeid() << ".sessionmap "


void SessionMap::init_inode()
{
  memset(&inode, 0, sizeof(inode));
  inode.ino = MDS_INO_SESSIONMAP_OFFSET + mds->get_nodeid();
  inode.layout = g_default_file_layout;
}


void SessionMap::dump()
{
  hash<entity_name_t> H;
  dout(0) << "dump" << dendl;
  for (hash_map<entity_name_t,Session*>::iterator p = session_map.begin();
       p != session_map.end();
       ++p) 
    dout(0) << p->first << " " << p->second << " hash " << H(p->first) << " addr " << (void*)&p->first << dendl;
}


// ----------------
// LOAD

class C_SM_Load : public Context {
  SessionMap *sessionmap;
public:
  bufferlist bl;
  C_SM_Load(SessionMap *cm) : sessionmap(cm) {}
  void finish(int r) {
	sessionmap->_load_finish(bl);
  }
};

void SessionMap::load(Context *onload)
{
  dout(10) << "load" << dendl;

  init_inode();

  if (onload)
	waiting_for_load.push_back(onload);
  
  C_SM_Load *c = new C_SM_Load(this);
  object_t oid(inode.ino, 0);
  mds->objecter->read(oid,
		      0, 0, // whole object
		      mds->objecter->osdmap->file_to_object_layout(oid,
								   g_default_mds_dir_layout),
		      &c->bl, 0,
		      c);

}

void SessionMap::_load_finish(bufferlist &bl)
{ 
  bufferlist::iterator blp = bl.begin();
  decode(blp);  // note: this sets last_cap_renew = now()
  dout(10) << "_load_finish v " << version 
	   << ", " << session_map.size() << " sessions, "
	   << bl.length() << " bytes"
	   << dendl;
  projected = committing = committed = version;
  dump();
  finish_contexts(waiting_for_load);
}


// ----------------
// SAVE

class C_SM_Save : public Context {
  SessionMap *sessionmap;
  version_t version;
public:
  C_SM_Save(SessionMap *cm, version_t v) : sessionmap(cm), version(v) {}
  void finish(int r) {
	sessionmap->_save_finish(version);
  }
};

void SessionMap::save(Context *onsave, version_t needv)
{
  dout(10) << "save needv " << needv << ", v " << version << dendl;
 
  if (needv && committing >= needv) {
    assert(committing > committed);
    commit_waiters[committing].push_back(onsave);
    return;
  }

  commit_waiters[version].push_back(onsave);
  
  bufferlist bl;
  
  init_inode();
  encode(bl);
  committing = version;
  SnapContext snapc;
  object_t oid(inode.ino, 0);
  mds->objecter->write_full(oid, 
			    mds->objecter->osdmap->file_to_object_layout(oid,
									 g_default_mds_dir_layout),
			    snapc,
			    bl, 0,
			    NULL, new C_SM_Save(this, version));
}

void SessionMap::_save_finish(version_t v)
{
  dout(10) << "_save_finish v" << v << dendl;
  committed = v;

  finish_contexts(commit_waiters[v]);
  commit_waiters.erase(v);
}


// -------------------

void SessionMap::encode(bufferlist& bl)
{
  ::encode(version, bl);
  __u32 n = session_map.size();
  ::encode(n, bl);
  for (hash_map<entity_name_t,Session*>::iterator p = session_map.begin(); 
       p != session_map.end(); 
       ++p) 
    p->second->encode(bl);
}

void SessionMap::decode(bufferlist::iterator& p)
{
  utime_t now = g_clock.now();

  ::decode(version, p);
  __u32 n;
  ::decode(n, p);
  while (n--) {
    Session *s = new Session;
    s->decode(p);
    session_map[s->inst.name] = s;
    s->last_cap_renew = now;
  }
}
