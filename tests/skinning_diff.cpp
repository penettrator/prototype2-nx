#include "generated/default/prototype2_pch.h"
#include <random>
#include <vector>
#include <chrono>
#include <iostream>
#include <cstring>
#include <cmath>
#include "p2_skinning_reference.inc"
#include "p2_skinning_kernel.inc"
#include "src/skinning_input_guard.h"
#include "src/p2_skinning_fast.h"
#include "src/native/skin_gpu_normals.h"
#include "src/native/skin_gpu_jobs.h"
#include <bit>
static void put32(std::vector<uint8_t>& m, size_t a, uint32_t v) {
  v = __builtin_bswap32(v); std::memcpy(m.data()+a, &v, 4);
}
// WORKLOG #92: split runner shape (reverse 32-vertex chunks, separately folded bounds, then merged).
// PERF pass 7: shape of p2skinpar::Parallel::Multi: every chunk folded from the caller's bounds, then
// combined in chunk order with min(acc, chunk) / max(acc, chunk) (later chunk wins ties).
struct MultiShape {
  template <class F> void operator()(uint32_t n, F&& range, simde__m128& v32, simde__m128& v33) const {
    const uint32_t chunks = (n + 31) / 32;
    std::vector<simde__m128> plo(chunks), phi(chunks);
    for (uint32_t c = chunks; c-- > 0;) {  // any completion order
      simde__m128 lo = v32, hi = v33;
      range(c * 32, std::min(n, c * 32 + 32), lo, hi);
      plo[c] = lo; phi[c] = hi;
    }
    simde__m128 lo = plo[0], hi = phi[0];
    for (uint32_t c = 1; c < chunks; ++c) { lo = simde_mm_min_ps(lo, plo[c]); hi = simde_mm_max_ps(hi, phi[c]); }
    v32 = lo; v33 = hi;
  }
};
struct Split {
  // The shape of p2skinpar::Parallel: owner [0, m) in order, helper chunks [m, n) from the back.
  template <class F> void operator()(uint32_t n, F&& range, simde__m128& v32, simde__m128& v33) const {
    const uint32_t chunks = (n + 31) / 32, m = chunks / 2 + (n % 3);  // uneven split points too
    const simde__m128 lo0 = v32, hi0 = v33;
    simde__m128 lo = lo0, hi = hi0;
    for (uint32_t c = chunks; c-- > std::min(m, chunks);) {
      simde__m128 clo = lo0, chi = hi0;
      range(c * 32, std::min(n, c * 32 + 32), clo, chi);
      lo = simde_mm_min_ps(clo, lo); hi = simde_mm_max_ps(chi, hi);
    }
    for (uint32_t c = 0; c < std::min(m, chunks); ++c) range(c * 32, std::min(n, c * 32 + 32), v32, v33);
    if (m < chunks) { v32 = simde_mm_min_ps(v32, lo); v33 = simde_mm_max_ps(v33, hi); }
  }
};
int main() {
  std::mt19937 random(415608);
  constexpr size_t memory_size = 0x100000;
  uint64_t original_ns = 0, local_ns = 0, fast_ns = 0, approx_ns = 0, positions_ns = 0, gpu_exact = 0, gpu_close = 0, jobs_covering = 0, jobs_emulated = 0, jobs_partial = 0, jobs_unaligned = 0, jobs_lazy = 0, jobs_lazy_stale = 0;
  const uint32_t sizes[] = {2, 3, 4, 7, 16, 31, 32, 129, 256, 1024};
  for (unsigned test = 0; test < 2000; ++test) {
    std::vector<uint8_t> a(memory_size, 0xA5);
    PPCContext ca{};
    // Randomize all registers, including dead lanes and high halves, to check exact preservation.
    for (size_t j=0; j<sizeof(ca); ++j) reinterpret_cast<uint8_t*>(&ca)[j]=uint8_t(random());
    ca.fpscr = {};
    const uint32_t n = sizes[test % std::size(sizes)];
    ca.r1.u64 = 0xF0000; ca.r3.u64 = 0x70000 + (test%16);
    ca.r4.u64 = 0x20000; ca.r5.u64 = n; ca.r6.u64 = 0x10000; ca.r7.u64=0xE0000;
    ca.lr=0x82123456;
    for (size_t i=0; i<256*16; ++i) {
      const float f = float(int32_t(random()%20001)-10000)/4096.0f;
      uint32_t bits; std::memcpy(&bits,&f,4); put32(a,0x10000+i*4,bits);
    }
    for (size_t i=0; i<n+2; ++i) {
      for (unsigned j=0;j<32;++j) a[0x20000+i*32+j]=uint8_t(random());
      for (unsigned j=0;j<3;++j) {
        float f=float(int32_t(random()%20001)-10000)/32.0f;
        uint32_t bits; std::memcpy(&bits,&f,4); put32(a,0x20000+i*32+j*4,bits);
      }
      for (unsigned j=0;j<4;++j) {
        const size_t p=0x20000+i*32+12+j*2;
        a[p]=0; a[p+1]=uint8_t(random());
      }
    }
    if (!P2SkinningFinitePositions(a.data()+ca.r4.u32,n)) return 2;
    auto b=a; auto c=a; auto sp=a; auto ap=a; auto mp=a; auto po=a; auto ps=a; auto pz=a; ca.fpscr.InitHost(); PPCContext cb=ca; PPCContext cc=ca; PPCContext cs=ca; PPCContext capx=ca; PPCContext cm=ca;
    const uint32_t host_start = ca.fpscr.getcsr();
    
    auto t0=std::chrono::steady_clock::now();
    p2_skinning_reference(ca,a.data());
    auto t1=std::chrono::steady_clock::now();
    cb.fpscr.setcsr(host_start);
    auto t2=std::chrono::steady_clock::now();
    p2_skinning_local(cb,b.data());
    auto t3=std::chrono::steady_clock::now();
    cc.fpscr.setcsr(host_start); cc.fpscr.enableFlushMode();
    auto t4=std::chrono::steady_clock::now();
    p2skin::Skin(c.data(), cc.r3.u32, cc.r4.u32, n, cc.r6.u32, cc.r7.u32, cc.r1.u32, uint32_t(cc.lr));
    auto t5=std::chrono::steady_clock::now();
    fast_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(t5-t4).count();
    {
      cs.fpscr.setcsr(host_start); cs.fpscr.enableFlushMode();
      p2skin::Skin(sp.data(), cs.r3.u32, cs.r4.u32, n, cs.r6.u32, cs.r7.u32, cs.r1.u32, uint32_t(cs.lr), UINT32_MAX, Split{});
      if (a!=sp) { std::cerr<<"SPLIT FAIL case="<<test<<" vertices="<<n; for(size_t j=0;j<a.size();++j) if(a[j]!=sp[j]) { std::cerr<<" offset="<<std::hex<<j<<" r7="<<cs.r7.u32<<" r3="<<cs.r3.u32<<" ref="<<int(a[j])<<" split="<<int(sp[j])<<std::dec; break; } std::cerr<<"\n"; return 6; }
    }
    {
      cm.fpscr.setcsr(host_start); cm.fpscr.enableFlushMode();
      p2skin::Skin(mp.data(), cm.r3.u32, cm.r4.u32, n, cm.r6.u32, cm.r7.u32, cm.r1.u32, uint32_t(cm.lr), UINT32_MAX, MultiShape{});
      if (a!=mp) { std::cerr<<"MULTI FAIL case="<<test<<" vertices="<<n<<"\n"; return 12; }
    }
    {
      // WORKLOG #99 approx: positions within 1e-4 relative (+1e-4 absolute), each 10-bit field of the packed
      // normal/tangent words within 2, bounds likewise; every other byte identical.
      PPCContext cx=capx; cx.fpscr.setcsr(host_start); cx.fpscr.enableFlushMode();
      const auto t6=std::chrono::steady_clock::now();
      p2skin::Skin(ap.data(), cx.r3.u32, cx.r4.u32, n, cx.r6.u32, cx.r7.u32, cx.r1.u32, uint32_t(cx.lr), UINT32_MAX, p2skin::Sequential{}, true);
      approx_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t6).count();
      const uint32_t out=cx.r3.u32, bb=cx.r7.u32&~15u;
      auto f32=[](const std::vector<uint8_t>& m,size_t o){uint32_t v=(uint32_t(m[o])<<24)|(uint32_t(m[o+1])<<16)|(uint32_t(m[o+2])<<8)|m[o+3]; float f; std::memcpy(&f,&v,4); return f;};
      auto u32=[](const std::vector<uint8_t>& m,size_t o){return (uint32_t(m[o])<<24)|(uint32_t(m[o+1])<<16)|(uint32_t(m[o+2])<<8)|m[o+3];};
      // Rounding of a weighted sum scales with its terms: tolerance relative to the call's coordinate range
      // (the reference bounds), 1e-5 of it.
      float range=0; for(int q=0;q<8;++q){ const float v=f32(a,bb+4*q); if(std::isfinite(v)&&std::fabs(v)<1e30f) range=std::max(range,std::fabs(v)); }
      auto close=[range](float x,float y){return std::fabs(x-y)<=1e-6f+1e-5f*range+1e-5f*std::fabs(y);};
      std::vector<uint8_t> mask(ap.size(),0);
      for(uint32_t k=0;k<n;++k){
        const size_t o=out+24*k;
        for(int q=0;q<3;++q){ if(!close(f32(ap,o+4*q),f32(a,o+4*q))){std::cerr<<"APPROX POS FAIL case="<<test<<" k="<<k<<" "<<f32(ap,o+4*q)<<" vs "<<f32(a,o+4*q)<<"\n";return 7;} }
        for(int w=0;w<2;++w){ const uint32_t x=u32(ap,o+16+4*w), y=u32(a,o+16+4*w);
          for(int sh=0;sh<30;sh+=10){ int dx=int((x>>sh)&1023), dy=int((y>>sh)&1023); if(std::abs(dx-dy)>2 && std::abs(dx-dy)<1022){std::cerr<<"APPROX NORMAL FAIL case="<<test<<" k="<<k<<"\n";return 8;} }
          if((x>>30)!=(y>>30)){std::cerr<<"APPROX W FAIL case="<<test<<"\n";return 9;} }
        for(size_t j=o;j<o+24;++j) mask[j]=1;
      }
      for(size_t j=out+24*n;j<out+24*n+8;++j) mask[j]=1;  // last vertex spill
      for(int q=0;q<8;++q){ if(!close(f32(ap,bb+4*q),f32(a,bb+4*q))){std::cerr<<"APPROX BBOX FAIL case="<<test<<"\n";return 10;} }
      for(size_t j=bb;j<bb+32;++j) mask[j]=1;
      for(size_t j=0;j<ap.size();++j) if(!mask[j]&&ap[j]!=a[j]){std::cerr<<"APPROX OTHER BYTE FAIL case="<<test<<" offset="<<std::hex<<j<<std::dec<<"\n";return 11;}
    }
    {
      // PERF pass 81 jobs: positions-only call, Publish, then Prepare over a covering range (job returned, filled
      // as the GPU would) or a partly covering one (filled into guest memory by Prepare): the full approx bytes.
      auto pj = pz;  // memory before any skinning
      bool skip_compare = false;
      PPCContext cx=capx; cx.fpscr.setcsr(host_start); cx.fpscr.enableFlushMode();
      p2skin::Skin(pj.data(), cx.r3.u32, cx.r4.u32, n, cx.r6.u32, cx.r7.u32, cx.r1.u32, uint32_t(cx.lr), UINT32_MAX, p2skin::Sequential{}, true, true);
      uint32_t mx = 0;
      for (uint32_t k = 0; k < n; ++k) for (int q = 0; q < 4; ++q) mx = std::max<uint32_t>(mx, p2skin::Load16BE(pj.data() + cx.r4.u32 + 32 * k + 12 + 2 * q));
      // PERF pass 85: cases 2,3 mod 4 publish lazy jobs (input read from "guest memory" at Prepare/patch time).
      const bool lazy = test % 4 >= 2;
      p2::skingpu::g_lazy_input.store(lazy);
      if (lazy) ++jobs_lazy;
      p2::skingpu::Publish(pj.data(), cx.r3.u32, cx.r4.u32, n, cx.r6.u32, mx, test);
      auto tr = [&](uint32_t phys) { return pj.data() + phys; };
      auto trv = [&](uint32_t in) -> const uint8_t* { return pj.data() + in; };

      std::vector<p2::skingpu::JobPtr> inside;
      const bool covering = test % 2 == 0;
      if (covering) {
        ++jobs_covering;
        p2::skingpu::Prepare(cx.r3.u32 & ~15u, 24 * n + 32, tr, trv, inside);
        if (inside.size() != 1) { std::cerr << "JOBS INSIDE FAIL case=" << test << " got " << inside.size() << "\n"; return 16; }
        // As PatchSkinned + skin_normals.cs.glsl: the range copied into a word stream, the job's input words and
        // rows memcpy'd after it, then the shader's reads (host-order input, host floats) and swapped writes.
        const auto& job = *inside[0];
        const uint32_t range = cx.r3.u32 & ~15u, range_bytes = 24 * n + 32;
        const size_t in_words = size_t(n - 1) * 8;
        if (job.lazy != lazy || (lazy && !job.input.empty())) { std::cerr << "JOBS LAZY FLAG FAIL case=" << test << "\n"; return 21; }
        std::vector<uint32_t> w((range_bytes + in_words * 4 + job.rows.size() * 4) / 4 + 4);
        std::memcpy(w.data(), pj.data() + range, range_bytes);
        const uint32_t in_word = (range_bytes + 15) / 16 * 4, rows_word = in_word + uint32_t(in_words);
        p2::skingpu::InputHost(job, pj.data() + job.in, &w[in_word]);
        std::memcpy(&w[rows_word], job.rows.data(), job.rows.size() * 4);
        if ((job.phys - range) % 4) { skip_compare = true; ++jobs_unaligned; }  // not Eligible: never GPU-patched
        else {
          ++jobs_emulated;
          const uint32_t dst_word = (job.phys - range) / 4, bones = uint32_t(job.rows.size() / 16);
          for (uint32_t k = 0; k + 1 < n; ++k) {
            const uint32_t base_w = in_word + k * 8;
            uint32_t q0[4] = {0, 0, 0, w[base_w + 3]}, q1[4];
            for (int q = 0; q < 4; ++q) q1[q] = w[base_w + 4 + q];
            for (uint32_t m4 : {q0[3] >> 16, q0[3] & 0xFFFFu, q1[0] >> 16, q1[0] & 0xFFFFu})
              if (m4 >= bones) { std::cerr << "JOBS BONE RANGE case=" << test << "\n"; return 19; }
            uint32_t words[2];
            p2skin_gpu::NormalWords(q0, q1, reinterpret_cast<const float*>(&w[rows_word]), words);
            w[dst_word + 6 * k + 4] = __builtin_bswap32(words[0]);
            w[dst_word + 6 * k + 5] = __builtin_bswap32(words[1]);
          }
          std::memcpy(pj.data() + range, w.data(), range_bytes);  // compare the patched copy below
        }
      } else {
        p2::skingpu::Prepare(cx.r3.u32 + 24, 24 * n, tr, trv, inside);  // starts inside the job's vertex 1
        ++jobs_partial;
        if (!inside.empty()) { std::cerr << "JOBS PARTIAL FAIL case=" << test << "\n"; return 17; }
      }
      if (!skip_compare && pj != ap) { std::cerr << "JOBS FILL FAIL case=" << test << " covering=" << covering << " n=" << n << " out=" << std::hex << cx.r3.u32; for (size_t j = 0; j < pj.size(); ++j) if (pj[j] != ap[j]) { std::cerr << " first=" << j << " pj=" << int(pj[j]) << " ap=" << int(ap[j]); break; } std::cerr << std::dec << "\n"; return 18; }
      if (lazy && test % 8 == 6) {
        // A lazy job whose sampled input changed after Publish is stale: no patch, no CPU fill. Control: the same
        // job over unchanged memory is returned.
        { std::lock_guard<std::mutex> lock(p2::skingpu::g_table.mutex); p2::skingpu::g_table.jobs.clear(); }
        p2::skingpu::Publish(pj.data(), cx.r3.u32, cx.r4.u32, n, cx.r6.u32, mx, test);
        auto changed = pj;
        changed[cx.r4.u32 + 32 * ((n - 2) / 2) + 13] ^= 0x40;
        std::vector<p2::skingpu::JobPtr> got;
        auto trc = [&](uint32_t phys) { return changed.data() + phys; };
        auto trvc = [&](uint32_t in) -> const uint8_t* { return changed.data() + in; };
        p2::skingpu::Prepare(cx.r3.u32 & ~15u, 24 * n + 32, trc, trvc, got);
        if (!got.empty()) { std::cerr << "JOBS LAZY STALE FAIL case=" << test << "\n"; return 20; }
        p2::skingpu::Publish(pj.data(), cx.r3.u32, cx.r4.u32, n, cx.r6.u32, mx, test);
        p2::skingpu::Prepare(cx.r3.u32 & ~15u, 24 * n + 32, tr, trv, got);
        if (got.size() != 1) { std::cerr << "JOBS LAZY CONTROL FAIL case=" << test << "\n"; return 22; }
        ++jobs_lazy_stale;
      }
      { std::lock_guard<std::mutex> lock(p2::skingpu::g_table.mutex); p2::skingpu::g_table.jobs.clear(); }
      p2::skingpu::g_lazy_input.store(false);
    }
    {
      // PERF pass 81: positions only (+ the call's last vertex in full), then the normal words of every other
      // vertex from ApproxNormalWords: byte-identical to the full approx call, sequential and split.
      for (int shape = 0; shape < 2; ++shape) {
        auto& m = shape ? ps : po;
        PPCContext cx=capx; cx.fpscr.setcsr(host_start); cx.fpscr.enableFlushMode();
        if (shape) p2skin::Skin(m.data(), cx.r3.u32, cx.r4.u32, n, cx.r6.u32, cx.r7.u32, cx.r1.u32, uint32_t(cx.lr), UINT32_MAX, Split{}, true, true);
        else { const auto tp=std::chrono::steady_clock::now();
          p2skin::Skin(m.data(), cx.r3.u32, cx.r4.u32, n, cx.r6.u32, cx.r7.u32, cx.r1.u32, uint32_t(cx.lr), UINT32_MAX, p2skin::Sequential{}, true, true);
          positions_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-tp).count(); }
        uint32_t mx = 0;
        for (uint32_t k = 0; k < n; ++k) for (int q = 0; q < 4; ++q) mx = std::max<uint32_t>(mx, p2skin::Load16BE(m.data() + cx.r4.u32 + 32 * k + 12 + 2 * q));
        std::vector<simde__m128> rows; p2skin::BuildRows(m.data(), cx.r6.u32, mx, rows);
        const p2skin::Consts cst = p2skin::MakeConsts();
        for (uint32_t k = 0; k + 1 < n; ++k) p2skin::ApproxNormalWords(m.data(), cx.r4.u32, k, rows.data(), cst, m.data() + cx.r3.u32 + 24 * k + 16);
        {
          // Scalar GPU reference (skin_gpu_normals.h) against ApproxNormalWords: fields within 2 codes.
          uint32_t q0[4], q1[4]; float rowsf[256 * 16];
          for (uint32_t b2 = 0; b2 <= mx; ++b2) for (int q = 0; q < 16; ++q) {
            const uint8_t* p = m.data() + cx.r6.u32 + b2 * 64 + q * 4;
            const uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
            rowsf[b2 * 16 + q] = std::bit_cast<float>(v);
          }
          for (uint32_t k = 0; k + 1 < n; ++k) {
            const uint8_t* v = m.data() + cx.r4.u32 + 32 * k;
            auto be = [](const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; };
            for (int q = 0; q < 4; ++q) { q0[q] = be(v + 4 * q); q1[q] = be(v + 16 + 4 * q); }
            uint32_t g[2]; p2skin_gpu::NormalWords(q0, q1, rowsf, g);
            const uint8_t* o = m.data() + cx.r3.u32 + 24 * k + 16;
            for (int wv = 0; wv < 2; ++wv) {
              const uint32_t x = g[wv], y = be(o + 4 * wv);
              if (x == y) { ++gpu_exact; continue; }
              ++gpu_close;
              for (int sh = 0; sh < 30; sh += 10) { const int dx = int((x >> sh) & 1023), dy = int((y >> sh) & 1023);
                if (std::abs(dx - dy) > 2 && std::abs(dx - dy) < 1022) { std::cerr << "GPU REF FAIL case=" << test << " k=" << k << std::hex << " " << x << " vs " << y << std::dec << "\n"; return 14; } }
              if ((x >> 30) != (y >> 30)) { std::cerr << "GPU REF TOP BITS FAIL case=" << test << "\n"; return 15; }
            }
          }
        }
        if (m != ap) {
          std::cerr<<"POSITIONS+NORMALS FAIL case="<<test<<" shape="<<shape<<" vertices="<<n;
          for(size_t j=0;j<m.size();++j) if(m[j]!=ap[j]) { std::cerr<<" offset="<<std::hex<<j<<" out="<<cx.r3.u32<<std::dec; break; }
          std::cerr<<"\n"; return 13;
        }
      }
    }
    if (a!=c) {
      std::cerr<<"FAST FAIL case="<<test<<" vertices="<<n<<" alignment="<<test%16<<"\n";
      for(size_t j=0;j<a.size();++j) if(a[j]!=c[j]) { std::cerr<<"memory offset="<<std::hex<<j<<" ref="<<int(a[j])<<" fast="<<int(c[j])<<std::dec<<"\n"; break; }
      return 5;
    }
    original_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t0).count();
    local_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(t3-t2).count();
    if (a!=b || std::memcmp(&ca,&cb,sizeof(ca))) {
      std::cerr<<"FAIL case="<<test<<" vertices="<<n<<" alignment="<<test%16<<"\n";
      for(size_t j=0;j<a.size();++j) if(a[j]!=b[j]) { std::cerr<<"memory offset="<<j<<"\n"; break; }
      for(size_t j=0;j<sizeof(ca);++j) if(reinterpret_cast<uint8_t*>(&ca)[j]!=reinterpret_cast<uint8_t*>(&cb)[j]) { std::cerr<<"context offset="<<j<<"\n"; break; }
      return 1;
    }
  }
  {
    std::vector<uint8_t> input(96,0);
    if(P2SkinningFinitePositions(input.data(),1)) return 3;
    for(uint32_t bad : {0x7F800000u,0xFF800000u,0x7FC00001u,0x7F800001u}) {
      put32(input,0,bad);
      if(P2SkinningFinitePositions(input.data(),2)) return 4;
    }
  }
  std::cout<<"PASS positions-only + ApproxNormalWords identical to approx (sequential and split); approx_ms="<<approx_ns/1e6<<" positions_only_ms="<<positions_ns/1e6<<" ratio="<<double(positions_ns)/approx_ns<<"; scalar GPU reference words exact "<<gpu_exact<<", within 2 codes "<<gpu_close<<"\n";
  std::cout<<"PASS jobs: covering "<<jobs_covering<<" (shader layout emulated "<<jobs_emulated<<", unaligned skipped "<<jobs_unaligned<<"), partial "<<jobs_partial<<", lazy "<<jobs_lazy<<" (input-change stale "<<jobs_lazy_stale<<")\n";
  std::cout<<"PASS non-finite and one-vertex input rejection\n";
  std::cout<<"PASS fast rewrite memory identical in all 2000 cases (also split into reverse chunks and per-chunk multi-helper folds); approx within tolerance, approx_ms="<<approx_ns/1e6<<"\n";
  std::cout<<"PASS 2000 differential cases: vertex counts 2..1024, all 16 output alignments, randomized matrices, packed inputs and register state\n";
  std::cout<<"host microbenchmark original_ms="<<original_ns/1e6<<" local_ms="<<local_ns/1e6<<" ratio="<<double(original_ns)/local_ns<<" fast_ms="<<fast_ns/1e6<<" fast_vs_local="<<double(local_ns)/fast_ns<<" (not a Switch FPS measurement)\n";
}
