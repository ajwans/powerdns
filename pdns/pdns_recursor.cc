/*
    PowerDNS Versatile Database Driven Nameserver
    Copyright (C) 2002  PowerDNS.COM BV

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "utility.hh" 
#include <iostream>
#include <errno.h>
#include <map>
#include <set>
#ifndef WIN32
#include <netdb.h>
#endif // WIN32
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include "mtasker.hh"
#include <utility>
#include "dnspacket.hh"
#include "statbag.hh"
#include "arguments.hh"
#include "syncres.hh"

#ifndef WIN32
extern "C" {
  int sem_init(sem_t*, int, unsigned int){return 0;}
  int sem_wait(sem_t*){return 0;}
  int sem_trywait(sem_t*){return 0;}
  int sem_post(sem_t*){return 0;}
  int sem_getvalue(sem_t*, int*){return 0;}
}
#endif // WIN32

StatBag S;
ArgvMap &arg()
{
  static ArgvMap theArg;
  return theArg;
}
int d_clientsock;
int d_serversock;

struct PacketID
{
  u_int16_t id;
  struct sockaddr_in remote;
};

bool operator<(const PacketID& a, const PacketID& b)
{
  if(a.id<b.id)
    return true;

  if(a.id==b.id) {
    if(a.remote.sin_addr.s_addr < b.remote.sin_addr.s_addr)
      return true;
    if(a.remote.sin_addr.s_addr == b.remote.sin_addr.s_addr)
      if(a.remote.sin_port < b.remote.sin_port)
	return true;
  }

  return false;
}

MTasker<PacketID,string> MT(100000); // could probably be way lower

/* these two functions are used by LWRes */
int asendto(const char *data, int len, int flags, struct sockaddr *toaddr, int addrlen, int id) 
{
  return sendto(d_clientsock, data, len, flags, toaddr, addrlen);
}

int arecvfrom(char *data, int len, int flags, struct sockaddr *toaddr, Utility::socklen_t *addrlen, int *d_len, int id)
{
  PacketID pident;
  pident.id=id;
  memcpy(&pident.remote,toaddr,sizeof(pident.remote));

  string packet;
  if(!MT.waitEvent(pident,&packet,1)) { // timeout
    return 0; 
  }

  *d_len=packet.size();
  memcpy(data,packet.c_str(),min(len,*d_len));

  return 1;
}

typedef map<string,set<DNSResourceRecord> > cache_t;
cache_t cache;
int cacheHits, cacheMisses;
int getCache(const string &qname, const QType& qt, set<DNSResourceRecord>* res)
{
  cache_t::const_iterator j=cache.find(toLower(qname)+"|"+qt.getName());
  if(j!=cache.end() && j->first==toLower(qname)+"|"+qt.getName() && j->second.begin()->ttl>(unsigned int)time(0)) {
    if(res)
      *res=j->second;

    return (unsigned int)j->second.begin()->ttl-time(0);
  }

  return -1;
}

void replaceCache(const string &qname, const QType& qt,  const set<DNSResourceRecord>& content)
{
  cache[toLower(qname)+"|"+qt.getName()]=content;
}

void init(void)
{
  // prime root cache
  static char*ips[]={"198.41.0.4", "128.9.0.107", "192.33.4.12", "128.8.10.90", "192.203.230.10", "192.5.5.241", "192.112.36.4", "128.63.2.53", 
		     "192.36.148.17","192.58.128.30", "193.0.14.129", "198.32.64.12", "202.12.27.33"};
  DNSResourceRecord arr, nsrr;
  arr.qtype=QType::A;
  arr.ttl=time(0)+3600000;
  nsrr.qtype=QType::NS;
  nsrr.ttl=time(0)+3600000;
  
  set<DNSResourceRecord>nsset;
  for(char c='a';c<='m';++c) {
    static char templ[40];
    strncpy(templ,"a.root-servers.net", sizeof(templ) - 1);
    *templ=c;
    arr.qname=nsrr.content=templ;
    arr.content=ips[c-'a'];
    set<DNSResourceRecord>aset;
    aset.insert(arr);
    replaceCache(string(templ),QType(QType::A),aset);

    nsset.insert(nsrr);
  }
  replaceCache("",QType(QType::NS),nsset);
}

void startDoResolve(void *p)
{
  try {
    bool quiet=arg().mustDo("quiet");
    DNSPacket P=*(DNSPacket *)p;

    delete (DNSPacket *)p;

    vector<DNSResourceRecord>ret;
    DNSPacket *R=P.replyPacket();
    R->setA(false);
    R->setRA(true);

    SyncRes sr;
    if(!quiet)
      L<<Logger::Error<<"["<<MT.getTid()<<"] question for '"<<P.qdomain<<"|"<<P.qtype.getName()<<"' from "<<P.getRemote()<<endl;

    sr.setId(MT.getTid());
    if(!P.d.rd)
      sr.setCacheOnly();

    int res=sr.beginResolve(P.qdomain, P.qtype, ret);
    if(res<0)
      R->setRcode(RCode::ServFail);
    else {
      R->setRcode(res);
      for(vector<DNSResourceRecord>::const_iterator i=ret.begin();i!=ret.end();++i)
	R->addRecord(*i);
    }

    const char *buffer=R->getData();
    sendto(d_serversock,buffer,R->len,0,(struct sockaddr *)(R->remote),R->d_socklen);
    if(!quiet) {
      L<<Logger::Error<<"["<<MT.getTid()<<"] answer to "<<(P.d.rd?"":"non-rd ")<<"question '"<<P.qdomain<<"|"<<P.qtype.getName();
      L<<"': "<<ntohs(R->d.ancount)<<" answers, "<<ntohs(R->d.arcount)<<" additional, took "<<sr.d_outqueries<<" packets, rcode="<<res<<endl;
    }
    
    sr.d_outqueries ? cacheMisses++ : cacheHits++;

    delete R;
  }
  catch(AhuException &ae) {
    L<<Logger::Error<<"startDoResolve problem: "<<ae.reason<<endl;
  }
  catch(...) {
    L<<Logger::Error<<"Any other exception in a resolver context"<<endl;
  }
}

void makeClientSocket()
{
  d_clientsock=socket(AF_INET, SOCK_DGRAM,0);
  if(d_clientsock<0) 
    throw AhuException("Making a socket for resolver: "+stringerror());
  
  struct sockaddr_in sin;
  memset((char *)&sin,0, sizeof(sin));
  
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = INADDR_ANY;
  
  int tries=10;
  while(--tries) {
    u_int16_t port=10000+Utility::random()%10000;
    sin.sin_port = htons(port); 
    
    if (bind(d_clientsock, (struct sockaddr *)&sin, sizeof(sin)) >= 0) 
      break;
    
  }
  if(!tries)
    throw AhuException("Resolver binding to local socket: "+stringerror());
}

void makeServerSocket()
{
  d_serversock=socket(AF_INET, SOCK_DGRAM,0);
  if(d_serversock<0) 
    throw AhuException("Making a server socket for resolver: "+stringerror());
  
  struct sockaddr_in sin;
  memset((char *)&sin,0, sizeof(sin));
  
  sin.sin_family = AF_INET;

  if(arg()["local-address"]=="0.0.0.0") {
    L<<Logger::Warning<<"It is advised to bind to explicit addresses with the --local-address option"<<endl;
    sin.sin_addr.s_addr = INADDR_ANY;
  }
  else {
    struct hostent *h=0;
    h=gethostbyname(arg()["local-address"].c_str());
    if(!h)
      throw AhuException("Unable to resolve local address"); 
    
    sin.sin_addr.s_addr=*(int*)h->h_addr;
  }

  sin.sin_port = htons(arg().asNum("local-port")); 
    
  if (bind(d_serversock, (struct sockaddr *)&sin, sizeof(sin))<0) 
    throw AhuException("Resolver binding to server socket: "+stringerror());
  L<<Logger::Error<<"Incoming query source port: "<<arg().asNum("local-port")<<endl;
}

#ifndef WIN32
void daemonize(void)
{
  if(fork())
    exit(0); // bye bye
  
  setsid(); 

  // cleanup open fds, but skip sockets 
  close(0);
  close(1);
  close(2);

}
#endif

int counter, qcounter;
bool statsWanted;

void usr1Handler(int)
{
  statsWanted=true;
}
void doStats(void)
{
  if(qcounter) {
    L<<Logger::Error<<"stats: "<<qcounter<<" questions, "<<cache.size()<<" cache entries, "<<SyncRes::s_negcache.size()<<" negative entries, "
     <<(int)((cacheHits*100.0)/(cacheHits+cacheMisses))<<"% cache hits";
    L<<Logger::Error<<", outpacket/query ratio "<<(int)(SyncRes::s_outqueries*100.0/SyncRes::s_queries)<<"%"<<endl;
  }
  statsWanted=false;
}

void houseKeeping(void *)
{
  static time_t last_stat, last_rootupdate;

  if(time(0)-last_stat>1800) { 
    doStats();
    last_stat=time(0);
  }
  if(time(0)-last_rootupdate>7200) {
    SyncRes sr;
    vector<DNSResourceRecord>ret;

    sr.setNoCache();
    int res=sr.beginResolve("", QType(QType::NS), ret);
    if(!res) {
      L<<Logger::Error<<"Refreshed . records"<<endl;
      last_rootupdate=time(0);
    }
    else
      L<<Logger::Error<<"Failed to update . records, RCODE="<<res<<endl;
  }
}

int main(int argc, char **argv) 
{
#ifdef WIN32
    WSADATA wsaData;
    WSAStartup( MAKEWORD( 2, 0 ), &wsaData );
#endif // WIN32

  try {
    Utility::srandom(time(0));
    arg().set("soa-minimum-ttl","Don't change")="0";
    arg().set("soa-serial-offset","Don't change")="0";
    arg().set("aaaa-additional-processing","turn on to do AAAA additional processing (slow)")="off";
    arg().set("local-port","port to listen on")="53";
    arg().set("local-address","port to listen on")="0.0.0.0";
    arg().set("trace","if we should output heaps of logging")="off";
    arg().set("daemon","Operate as a daemon")="yes";
    arg().set("quiet","Suppress logging of questions and answers")="off";
    arg().set("config-dir","Location of configuration directory (recursor.conf)")=SYSCONFDIR;
    arg().setCmd("help","Provide a helpful message");
    L.toConsole(Logger::Warning);
    arg().laxParse(argc,argv); // do a lax parse

    string configname=arg()["config-dir"]+"/recursor.conf";
    cleanSlashes(configname);

    if(!arg().file(configname.c_str())) 
      L<<Logger::Warning<<"Unable to parse configuration file '"<<configname<<"'"<<endl;

    arg().parse(argc,argv);

    if(arg().mustDo("help")) {
      cerr<<"syntax:"<<endl<<endl;
      cerr<<arg().helpstring(arg()["help"])<<endl;
      exit(99);
    }

    L.setName("pdns_recursor");

    if(arg().mustDo("trace"))
      SyncRes::setLog(true);
    
    makeClientSocket();
    makeServerSocket();
        
    char data[1500];
    struct sockaddr_in fromaddr;
    
    PacketID pident;
    init();    
    L<<Logger::Warning<<"Done priming cache with root hints"<<endl;
#ifndef WIN32
    if(arg().mustDo("daemon")) {
      L.toConsole(Logger::Critical);
      daemonize();
    }
    signal(SIGUSR1,usr1Handler);
#endif

    for(;;) {
      while(MT.schedule()); // housekeeping, let threads do their thing
      
      if(!((counter++)%100)) 
	MT.makeThread(houseKeeping,0);
      if(statsWanted)
	doStats();

      Utility::socklen_t addrlen=sizeof(fromaddr);
      int d_len;
      DNSPacket P;
      
      struct timeval tv;
      tv.tv_sec=0;
      tv.tv_usec=500000;
      
      fd_set readfds;
      FD_ZERO( &readfds );
      FD_SET( d_clientsock, &readfds );
      FD_SET( d_serversock, &readfds );
      int selret = select( max(d_clientsock,d_serversock) + 1, &readfds, NULL, NULL, &tv );
      if(selret<=0) 
	if (selret == -1 && errno!=EINTR) 
	  throw AhuException("Select returned: "+stringerror());
	else
	  continue;

      if(FD_ISSET(d_clientsock,&readfds)) { // do we have a question response?
	d_len=recvfrom(d_clientsock, data, sizeof(data), 0, (sockaddr *)&fromaddr, &addrlen);    
	if(d_len<0) 
	  continue;
	
	P.setRemote((struct sockaddr *)&fromaddr, addrlen);
	if(P.parse(data,d_len)<0) {
	  L<<Logger::Error<<"Unparseable packet from remote server "<<P.getRemote()<<endl;
	}
	else { 
	  if(P.d.qr) {

	    pident.remote=fromaddr;
	    pident.id=P.d.id;
	    string packet;
	    packet.assign(data,d_len);
	    MT.sendEvent(pident,&packet);
	  }
	  else 
	    L<<Logger::Warning<<"Ignoring question on outgoing socket from "<<P.getRemote()<<endl;
	}
      }
      
      if(FD_ISSET(d_serversock,&readfds)) { // do we have a new question?
	d_len=recvfrom(d_serversock, data, sizeof(data), 0, (sockaddr *)&fromaddr, &addrlen);    
	if(d_len<0) 
	  continue;
 	P.setRemote((struct sockaddr *)&fromaddr, addrlen);
	if(P.parse(data,d_len)<0) {
	  L<<Logger::Error<<"Unparseable packet from remote client "<<P.getRemote()<<endl;
	}
	else { 
	  if(P.d.qr)
	    L<<Logger::Error<<"Ignoring answer on server socket!"<<endl;
	  else {
	    ++qcounter;
	    MT.makeThread(startDoResolve,(void*)new DNSPacket(P));

	  }
	}
      }
    }
  }
  catch(AhuException &ae) {
    L<<Logger::Error<<"Exception: "<<ae.reason<<endl;
  }
  catch(exception &e) {
    L<<Logger::Error<<"STL Exception: "<<e.what()<<endl;
  }
  catch(...) {
    L<<Logger::Error<<"any other exception in main: "<<endl;
  }
  
#ifdef WIN32
  WSACleanup();
#endif // WIN32

  return 0;
}
