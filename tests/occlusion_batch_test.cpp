#include "src/native/occlusion_tracker.h"
#include "src/native/occlusion_batch.h"
#include "src/native/texture_eviction.h"
#include <cassert>
#include <vector>
#include <map>
#include <algorithm>
#include <random>
#include <cstdio>
using namespace p2::native;
using Tracker=OcclusionTracker<256>;
struct Piece { Tracker::Owners owners; uint64_t samples=0; };
struct Driver {
 Tracker tracker; OcclusionCommandState state; Tracker::Owners previous{};
 std::vector<Piece> pieces; uint32_t batch=Tracker::invalid,current=Tracker::invalid;
 uint32_t begins=0,ends=0;
 void Close(){state.Close([&](uint32_t i){assert(current==i);current=Tracker::invalid;++ends;});batch=Tracker::invalid;previous={};}
 void Draw(uint32_t samples){
  auto owners=tracker.OpenOwners(); uint32_t index=Tracker::invalid;
  if(owners.count){
   bool same=batch!=Tracker::invalid && owners.count==previous.count;
   for(uint32_t i=0;same && i<owners.count;++i)same=owners.ids[i]==previous.ids[i];
   if(!same){previous=owners;batch=uint32_t(pieces.size());pieces.push_back({tracker.AddOpenPiece(),0});}
   index=batch;
  }else{batch=Tracker::invalid;previous={};}
  state.Set(index,[&](uint32_t i){assert(current==i);current=Tracker::invalid;++ends;},
                  [&](uint32_t i){assert(current==Tracker::invalid);current=i;++begins;});
  if(current!=Tracker::invalid)pieces[current].samples+=samples;
 }
};
int main(){
 {Driver d;assert(d.tracker.Begin(0x1020));auto id=d.tracker.active();
  for(int i=0;i<5000;++i)d.Draw(i%3);
  assert(d.pieces.size()==1 && d.begins==1 && d.ends==0);
  assert(d.tracker.EndMatching(0x1000)==id);d.Close();assert(d.ends==1);
  for(auto p:d.pieces)for(uint32_t j=0;j<p.owners.count;++j)d.tracker.Complete(p.owners.ids[j],p.samples);
  Tracker::Result result;assert(d.tracker.TakeReady(id,result));assert(!result.fallback && result.samples==4999);
 }
 {Tracker t;assert(t.BeginOverlapping(0x2020));auto old=t.active();t.AddPiece(old);t.EndMatching(0x2000);
  assert(t.BeginOverlapping(0x2020));auto newer=t.active();t.EndMatching(0x2000);
  Tracker::Result r;assert(t.TakeReady(newer,r) && !r.superseded);
  t.Complete(old,123);assert(t.TakeReady(old,r) && r.superseded);
 }
 std::mt19937 rng(241015);
 for(int trial=0;trial<1000;++trial){
  Driver d;std::map<uint32_t,uint32_t> active;std::map<uint32_t,uint64_t> expected;
  uint32_t next=0x1000;
  for(int step=0;step<1000;++step){
   unsigned event=rng()%10;
   if(event==0 && active.size()<16 && expected.size()<200){
    uint32_t address=next;next+=64;assert(d.tracker.BeginOverlapping(address+32));uint32_t id=d.tracker.active();active[address]=id;expected[id]=0;
   }else if(event==1 && !active.empty()){
    auto it=active.begin();std::advance(it,rng()%active.size());assert(d.tracker.EndMatching(it->first)==it->second);active.erase(it);
   }else if(event==2){d.Close();}
   else{uint32_t n=rng()%101;for(auto [address,id]:active)expected[id]+=n;d.Draw(n);}
  }
  for(auto [address,id]:active)assert(d.tracker.EndMatching(address)==id);
  d.Close();assert(d.begins==d.ends);
  std::shuffle(d.pieces.begin(),d.pieces.end(),rng);
  for(auto& p:d.pieces)for(uint32_t j=0;j<p.owners.count;++j)assert(d.tracker.Complete(p.owners.ids[j],p.samples));
  for(auto [id,samples]:expected){Tracker::Result r;assert(d.tracker.TakeReady(id,r));assert(!r.fallback && !r.superseded && r.samples==samples);}
  assert(!d.tracker.pending());
 }
 assert(!IdleTextureExpired(600,0,false));
 assert(IdleTextureExpired(601,0,false));
 assert(!IdleTextureExpired(10000,0,true));
 assert(!IdleTextureExpired(100,200,false));
 assert(IsSmallColorLut(true,true,32,32,32,131072));
 assert(IsSmallColorLut(true,true,16,16,16,16384));
 assert(!IsSmallColorLut(false,true,32,32,32,131072));
 assert(!IsSmallColorLut(true,false,32,32,32,131072));
 assert(!IsSmallColorLut(true,true,64,64,64,1048576));
 assert(!IsSmallColorLut(true,true,32,16,32,65536));
 assert(!IsSmallColorLut(true,true,32,32,32,524288));
 struct Memo { unsigned view;void* texture;uint64_t valid_until; };
 int old=0,other=0;Memo m{123,&old,999999},n{456,&other,888888},resolved{789,nullptr,100};
 auto evicted=[&](const void* p){return p==&old;};
 assert(InvalidateEvictedTextureMemo(m,evicted));assert(!m.view && !m.texture && !m.valid_until);
 assert(!InvalidateEvictedTextureMemo(n,evicted) && n.view==456 && n.texture==&other);
 assert(!InvalidateEvictedTextureMemo(resolved,evicted) && resolved.view==789);
 std::puts("PASS 1,000 randomized overlapping-query streams, 5,000-draw exact long query, recorder scope balance, out-of-order completions, retired-generation guard and texture memo eviction");
}
