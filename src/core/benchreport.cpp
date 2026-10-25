// Benchmark outputs (bench.cpp collects the checkpoints): <base>_benchmark.csv (one row per checkpoint, ';' and
// decimal comma like the performance log) and <base>_benchmark.html: a results table (GPU time, the mod's cost per
// eye, VRAM, shimmer, each against the game's TAA) and a comparison viewer. The viewer takes any checkpoint for the
// left and the right side (TAA or a model at a quality); an orange line sweeps across (left of it = the left choice)
// over the looping footage, the shimmer maps or the screenshots. No game code here, so tests/benchtest.cpp builds it
// as is.
#include "common.h"
#include <string>
#include <cstdarg>

namespace {

void BrUtf8(const wchar_t* w, char* out, int n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, nullptr, nullptr); }

// decimal comma for the CSV (Excel / LibreOffice in Portuguese, like the performance log)
std::string Num(double v, int dec) {
    char b[48]; snprintf(b, sizeof(b), "%.*f", dec, v);
    for (char* c = b; *c; ++c) if (*c == '.') *c = ',';
    return b;
}

// escapes for a JavaScript literal: only plain characters expected (labels, file names), others dropped
std::string Js(const char* s) {
    std::string o;
    for (const char* c = s; *c; ++c) {
        if (*c == '\\' || *c == '\'') { o += '\\'; o += *c; }
        else if ((unsigned char)*c >= 32 && *c != '<' && *c != '>') o += *c;
    }
    return o;
}

}   // namespace

bool BenchWriteReport(const wchar_t* base, const std::vector<BenchPoint>& pts, const char* settings) {
    if (pts.empty()) return false;
    int baseIdx = -1;
    for (size_t i = 0; i < pts.size(); ++i) if (pts[i].taa) { baseIdx = (int)i; break; }
    const BenchPoint* b0 = baseIdx >= 0 ? &pts[baseIdx] : nullptr;

    // ---- CSV
    wchar_t csvPath[MAX_PATH]; swprintf_s(csvPath, L"%s_benchmark.csv", base);
    bool csvOk = false;
    if (FILE* f = _wfopen(csvPath, L"wb")) {
        fputs("index;label;model;quality;render_pct;render_w;render_h;out_w;out_h;gpu_ms_median;gpu_ms_p90;gpu_ms_vs_taa;"
              "frame_ms_median;fps;late_pct;game_thread_ms;render_thread_ms;mod_gpu_ms_eye0;mod_gpu_ms_eye1;vram_mb;vram_vs_taa_mb;"
              "shimmer_mean;shimmer_p95;shimmer_pct_over2;samples;screenshot;frames;shimmer_map;"
              "vram_before_mb;vram_own_mb;vram_peak_mb;vram_peak_own_mb;settle_s\r\n", f);
        for (size_t i = 0; i < pts.size(); ++i) {
            const BenchPoint& p = pts[i];
            std::string row = std::to_string(i) + ";" + p.label + ";" + p.model + ";" + p.quality + ";" + Num(p.pct, 1) + ";" +
                std::to_string(p.renderW) + ";" + std::to_string(p.renderH) + ";" + std::to_string(p.outW) + ";" + std::to_string(p.outH) + ";" +
                Num(p.gpuMed, 3) + ";" + Num(p.gpuP90, 3) + ";" + (b0 ? Num(p.gpuMed - b0->gpuMed, 3) : std::string()) + ";" +
                Num(p.frameMed, 3) + ";" + Num(p.fps, 2) + ";" + Num(p.latePct, 2) + ";" + Num(p.gameMed, 3) + ";" + Num(p.renderMed, 3) + ";" +
                Num(p.mod0, 3) + ";" + Num(p.mod1, 3) + ";" + Num(p.vramMb, 0) + ";" + (b0 ? Num(p.vramMb - b0->vramMb, 0) : std::string()) + ";" +
                (p.hasBurst ? Num(p.burst.flicker, 3) : std::string()) + ";" + (p.hasBurst ? Num(p.burst.p95, 3) : std::string()) + ";" +
                (p.hasBurst ? Num(p.burst.over2 * 100.0, 2) : std::string()) + ";" + std::to_string(p.samples) + ";" +
                p.shot + ";" + p.frames + ";" + p.heat + ";" +
                (p.vramOff > 0 ? Num(p.vramOff, 0) : std::string()) + ";" + (p.vramOff > 0 ? Num(p.vramMb - p.vramOff, 0) : std::string()) + ";" +
                Num(p.vramPeak, 0) + ";" + (p.vramOff > 0 ? Num(p.vramPeak - p.vramOff, 0) : std::string()) + ";" + Num(p.settleS, 1) + "\r\n";
            fputs(row.c_str(), f);
        }
        fclose(f); csvOk = true;
    }

    // ---- page
    char title[MAX_PATH * 3]; { const wchar_t* s = wcsrchr(base, L'\\'); BrUtf8(s ? s + 1 : base, title, sizeof(title)); }
    char csv8[MAX_PATH * 3]; { const wchar_t* s = wcsrchr(csvPath, L'\\'); BrUtf8(s ? s + 1 : csvPath, csv8, sizeof(csv8)); }
    int W = 0, H = 0, M = 0;   // burst size and the margin that keeps every lined-up layer inside its image
    for (const BenchPoint& p : pts) if (p.hasBurst) {
        if (!W) { W = p.burst.w; H = p.burst.h; }
        if (p.burst.w == W && p.burst.h == H) M = (std::max)(M, (std::max)(abs(p.burst.dx), abs(p.burst.dy)));
    }
    if (W && (W - 2 * M < 64 || H - 2 * M < 64)) M = 0;
    std::string s;
    char buf[2048];
    auto add = [&](const char* fmt, ...) { va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap); s += buf; };
    add("<!doctype html><html><head><meta charset=\"utf-8\"><title>%s benchmark</title><style>\n", title);
    s += R"CSS(html,body{margin:0;background:#111;color:#ddd;font:14px system-ui,sans-serif}
.wrap{padding:12px 16px;display:flex;flex-direction:column;gap:12px}
h1{font-size:18px;margin:0}h2{font-size:16px;margin:8px 0 0}
p{margin:0;color:#aaa;max-width:120ch}a{color:#f5b335}
table{border-collapse:collapse;font-variant-numeric:tabular-nums}
th,td{padding:4px 10px;border-bottom:1px solid #2a2a2a;text-align:right;white-space:nowrap}
th{color:#bbb;font-weight:600;cursor:pointer;position:sticky;top:0;background:#161616}
td:first-child,th:first-child,td:nth-child(2),th:nth-child(2){text-align:left}
tr.taa td{color:#9fd3ff}tr.sel td{background:#2b2410}tr:hover td{background:#1d1d1d}
.up{color:#ff8a7a}.dn{color:#8be38b}
.bar{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
select,button{background:#2a2a2a;color:#eee;border:1px solid #444;border-radius:4px;padding:5px 10px;font:inherit}
button{cursor:pointer}button.on{background:#f5b335;color:#111;border-color:#f5b335}
.wipe{position:relative;overflow:hidden;border:1px solid #333;cursor:ew-resize;background:#000}
.wipe .layer{position:absolute;inset:0;background-repeat:no-repeat}
.wipe .line{position:absolute;top:0;bottom:0;width:3px;margin-left:-1px;background:#f5b335;pointer-events:none}
.wipe .tag{position:absolute;top:8px;padding:3px 8px;background:rgba(0,0,0,.7);border-radius:4px;font-weight:600;pointer-events:none}
#sum{color:#ccc}#none{padding:40px;color:#aaa}
</style></head><body><div class="wrap">
)CSS";
    add("<h1>Benchmark %s</h1>\n<p>%s</p>\n", title, settings);
    add("<p>Each checkpoint: a settle wait, then GPU time and VRAM measured with the view still, then a short burst of frames "
        "(a crop around the centre of the view) and a full-resolution screenshot. Deltas are against the game's TAA (first row). "
        "VRAM is the whole game process. Click a row to compare it with TAA. Data: <a href=\"%s\">%s</a></p>\n", csv8, csv8);
    s += R"HTML(<p class="models"><b>The models.</b> Every DLSS model runs in the same NVIDIA DLSS library (version 310.9.16, bundled with the mod); the letter picks the neural network.<br>
<b>E</b>: the convolutional (CNN) model of DLSS 3, the last one before the transformers: the softest and the cheapest.<br>
<b>J, K</b>: DLSS 4, first-generation transformer model: sharper, more expensive (J and K measure the same here; NVIDIA recommends K).<br>
<b>L, M</b>: DLSS 4.5, second-generation transformer model: the sharpest and cleanest, the most expensive (best on RTX 40/50).<br>
<b>FSR</b>: AMD FSR 3.1.5 (the version this NVIDIA card runs; it works on any DirectX 12 GPU). The mod would use FSR 4 on Radeon RX 9000 cards, which is untested as of yet.<br>
<b>TAA</b>: the game's own anti-aliasing, without the mod.</p>
<p class="shimmer"><b>Shimmer</b> is how much the image flickers from one frame to the next while your head is still. In VR you see it as crawling or sparkling on fine detail: foliage, wires, fences, thin edges, distant textures.</p>
)HTML";
    s += R"HTML(<div style="overflow-x:auto;max-width:100%"><table id="t"><thead><tr><th>#</th><th>Setting</th><th>Render per eye</th><th>GPU ms</th><th>vs TAA</th><th>GPU p90</th>
<th>VRAM MB</th><th>vs TAA</th>
<th>Shimmer</th><th>vs TAA</th></tr></thead><tbody></tbody></table></div>
<h2>Compare</h2>
<div class="bar">Left of the line: <select id="sl"></select> Right of the line: <select id="sr"></select>
<button id="sw" title="X">Swap</button>
<button data-m="f">Footage</button><button data-m="h" class="on">Shimmer map</button><button data-m="s">Screenshot</button><button id="au" class="on" title="A: switch between the shimmer map and the footage after every sweep of the line">Auto</button>
<button id="pp">Pause</button><button data-s="1" class="on">Real speed</button><button data-s="0.5">1/2</button>
<button data-s="0.25">1/4</button><button data-s="0.1">1/10</button><button id="zm">Zoom 2x</button><span id="fr"></span></div>
<div id="sum"></div>
<div class="wipe" id="wp"><div class="layer" id="lr"></div><div class="layer" id="ll"></div><div class="line" id="ln"></div>
<div class="tag" id="tl" style="left:8px"></div><div class="tag" id="tr" style="right:8px"></div></div>
<div id="none" style="display:none">No footage for this pair (the burst failed or was turned off).</div>
<p>The line sweeps by itself; move the mouse over the picture to hold it there. Space: pause &middot; Left/Right: one frame
&middot; Z: zoom 2x &middot; X: swap sides &middot; 1/2/3: footage / shimmer map / screenshot &middot; A: auto (shimmer map and footage take turns after every sweep). Shimmer map: bright = the pixel
changes from frame to frame while the view is still (flicker, crawling edges). Footage and maps are lined up with the first
burst; screenshots are shown as taken.</p>
</div><script>
)HTML";
    // data
    add("var W=%d,H=%d,M=%d,B=%d;\nvar P=[\n", W, H, M, baseIdx);
    for (size_t i = 0; i < pts.size(); ++i) {
        const BenchPoint& p = pts[i];
        add("{vo:%.0f,vp:%.0f,st:%.1f,", p.vramOff, p.vramPeak, p.settleS);
        add("l:'%s',taa:%d,pct:%.1f,rw:%d,rh:%d,ow:%d,oh:%d,g:%.3f,g9:%.3f,fm:%.3f,fps:%.2f,late:%.2f,m0:%.3f,m1:%.3f,v:%.0f,"
            "b:%d,n:%d,dx:%d,dy:%d,bf:%.2f,fl:%.3f,f95:%.3f,fo:%.4f,sh:'%s',fr:'%s',ht:'%s'}%s\n",
            Js(p.label).c_str(), p.taa ? 1 : 0, p.pct, p.renderW, p.renderH, p.outW, p.outH, p.gpuMed, p.gpuP90, p.frameMed, p.fps,
            p.latePct, p.mod0, p.mod1, p.vramMb, p.hasBurst && p.burst.w == W && p.burst.h == H ? 1 : 0, p.burst.n, p.burst.dx, p.burst.dy,
            p.burst.fps, p.burst.flicker, p.burst.p95, p.burst.over2, p.hasShot ? Js(p.shot).c_str() : "", Js(p.frames).c_str(),
            Js(p.heat).c_str(), i + 1 < pts.size() ? "," : "");
    }
    s += "];\n";
    s += R"JS(function $(i){return document.getElementById(i);}
var tb=document.querySelector('#t tbody'),sl=$('sl'),sr=$('sr'),wp=$('wp'),lr=$('lr'),ll=$('ll'),ln=$('ln'),tl=$('tl'),tr=$('tr');
var mode='h',k=0,speed=1,zoom=1,run=true,last=0,acc=0,wt=0,hold=-1,scale=1,shots={},auto=true,acyc=0;
var R=B>=0?P[B]:null;
function f(v,d){return v.toFixed(d);}
function delta(v,ref,d,unit){if(ref==null)return '';var x=v-ref;var c=Math.abs(x)<Math.pow(10,-d)/2?'':(x>0?'up':'dn');return '<span class="'+c+'">'+(x>0?'+':'')+f(x,d)+(unit||'')+'</span>';}
function pctd(v,ref){if(!ref||ref<=0)return '';var x=(v/ref-1)*100;return '<span class="'+(x>0.5?'up':x<-0.5?'dn':'')+'">'+(x>0?'+':'')+f(x,0)+' %</span>';}
P.forEach(function(p,i){
 var tr=document.createElement('tr');if(p.taa)tr.className='taa';tr.dataset.i=i;
 var rs=p.taa?(p.ow?p.ow+'x'+p.oh+' (native)':'native'):(p.rw+'x'+p.rh+' ('+f(p.pct,1)+' %)');
 tr.innerHTML='<td>'+i+'</td><td>'+p.l+'</td><td>'+rs+'</td><td>'+f(p.g,2)+'</td><td>'+(R&&i!=B?delta(p.g,R.g,2,' ms'):'')+'</td><td>'+f(p.g9,2)+
  '</td><td>'+f(p.v,0)+'</td><td>'+(R&&i!=B?delta(p.v,R.v,0,' MB'):'')+
  '</td><td>'+(p.b?f(p.fl,2):'')+'</td><td>'+(R&&i!=B&&p.b&&R.b?pctd(p.fl,R.fl):'')+
  '</td>';
 tr.onclick=function(){if(B>=0)sl.value=B;sr.value=i;changed();};
 tb.appendChild(tr);
 [sl,sr].forEach(function(s){var o=document.createElement('option');o.value=i;o.textContent=i+': '+p.l;s.appendChild(o);});
});
var md=P.findIndex(function(p){return p.l=='DLSS-M-DLAA';});if(md>=0&&B>=0){sl.value=md;sr.value=B;}else{sl.value=B>=0?B:0;var first=P.findIndex(function(p,i){return !p.taa;});sr.value=first>=0?first:(P.length>1?1:0);}
function L(){return P[+sl.value];}function Rt(){return P[+sr.value];}
function loadShot(p,cb){if(!p.sh){cb(null);return;}if(shots[p.sh]){cb(shots[p.sh]);return;}var im=new Image();im.onload=function(){shots[p.sh]=im;cb(im);};im.onerror=function(){cb(null);};im.src=p.sh;}
function view(){var a=L(),b=Rt();
 if(mode=='s'){var im=shots[a.sh]||shots[b.sh];return im?{w:im.naturalWidth,h:im.naturalHeight}:null;}
 if(!a.b||!b.b)return null;return {w:W-2*M,h:H-2*M};}
function layout(){var v=view();
 $('none').textContent=mode=='s'?'No screenshot for this pair (it failed or was turned off).':'No footage for this pair (the burst failed or was turned off).';
 $('none').style.display=v?'none':'block';wp.style.display=v?'block':'none';if(!v)return;
 var avail=Math.max(200,document.body.clientWidth-40),availH=Math.max(200,window.innerHeight-160);
 scale=Math.min(1,avail/v.w,mode=='s'?availH/v.h:1)*zoom;
 wp.style.width=(v.w*scale)+'px';wp.style.height=(v.h*scale)+'px';
 [[ll,L()],[lr,Rt()]].forEach(function(x){var e=x[0],p=x[1];e.style.imageRendering=zoom>1?'pixelated':'auto';
  if(mode=='s'){e.style.backgroundImage=p.sh?'url('+p.sh+')':'none';e.style.backgroundSize=(v.w*scale)+'px '+(v.h*scale)+'px';}
  else if(mode=='h'){e.style.backgroundImage='url('+p.ht+')';e.style.backgroundSize=(W*scale)+'px '+(H*scale)+'px';}
  else{e.style.backgroundImage='url('+p.fr+')';e.style.backgroundSize=(W*scale)+'px '+(H*p.n*scale)+'px';}});
 tl.textContent=L().l;tr.textContent=Rt().l;draw();}
function draw(){if(wp.style.display=='none')return;
 [[ll,L()],[lr,Rt()]].forEach(function(x){var e=x[0],p=x[1];
  if(mode=='s')e.style.backgroundPosition='0 0';
  else{var kk=mode=='f'?(k%p.n):0;e.style.backgroundPosition=(-(M+p.dx)*scale)+'px '+(-(M+p.dy+kk*H)*scale)+'px';}});
 var cyc=4.5,t=wt%cyc,pos=t<0.5?0:t<3.5?(t-0.5)/3:1,q=hold>=0?hold:pos;
 ll.style.clipPath='inset(0 '+((1-q)*100)+'% 0 0)';ln.style.left=(q*100)+'%';
 $('fr').textContent=mode=='f'?('frame '+(k%L().n+1)+' / '+L().n):'';}
function summary(){var a=L(),b=Rt();
 $('sum').innerHTML='Right (<b>'+b.l+'</b>) compared with left (<b>'+a.l+'</b>): GPU '+f(b.g,2)+' ms ('+delta(b.g,a.g,2,' ms')+'), VRAM '+f(b.v,0)+
  ' MB ('+delta(b.v,a.v,0,' MB')+')'+(a.b&&b.b?', shimmer '+f(b.fl,2)+' ('+pctd(b.fl,a.fl)+')':'');
 document.querySelectorAll('#t tbody tr').forEach(function(r){r.classList.toggle('sel',+r.dataset.i==+sl.value||+r.dataset.i==+sr.value);});}
function changed(){summary();if(mode=='s'){var n=0;[L(),Rt()].forEach(function(p){loadShot(p,function(){if(++n==2)layout();});});}else layout();}
function fps(){var a=L(),b=Rt(),v=(a.bf+b.bf)/2;return v>1?v:72;}
function tick(t){if(last&&run){var dt=t-last;acc+=dt*speed;wt+=dt/1000;if(auto){var c=Math.floor(wt/4.5);if(c!=acyc){acyc=c;if(hold<0&&mode!='s')setMode(mode=='h'?'f':'h',true);}}var d=1000/fps();while(acc>=d){acc-=d;k++;}draw();}last=t;requestAnimationFrame(tick);}
wp.onmousemove=function(e){var r=wp.getBoundingClientRect();hold=Math.max(0,Math.min(1,(e.clientX-r.left)/r.width));draw();};
wp.onmouseleave=function(){hold=-1;};
sl.onchange=changed;sr.onchange=changed;
$('sw').onclick=function(){var x=sl.value;sl.value=sr.value;sr.value=x;changed();};
function setAuto(on){auto=on;acyc=Math.floor(wt/4.5);$('au').classList.toggle('on',on);}
function setMode(m,fromAuto){if(!fromAuto&&auto)setAuto(false);mode=m;document.querySelectorAll('button[data-m]').forEach(function(x){x.classList.toggle('on',x.dataset.m==m);});changed();}
document.querySelectorAll('button[data-m]').forEach(function(b){b.onclick=function(){setMode(b.dataset.m);};});
document.querySelectorAll('button[data-s]').forEach(function(b){b.onclick=function(){speed=+b.dataset.s;
 document.querySelectorAll('button[data-s]').forEach(function(x){x.classList.toggle('on',x===b);});};});
var pp=$('pp'),zm=$('zm');
pp.onclick=function(){run=!run;pp.textContent=run?'Pause':'Play';};
zm.onclick=function(){zoom=zoom>1?1:2;zm.classList.toggle('on',zoom>1);layout();};
$('au').onclick=function(){setAuto(!auto);if(auto&&mode=='s')setMode('h',true);};
window.onkeydown=function(e){if(e.target.tagName=='SELECT')return;
 if(e.key==' '){e.preventDefault();pp.onclick();}
 else if(e.key=='ArrowRight'){run=false;pp.textContent='Play';k++;draw();}
 else if(e.key=='ArrowLeft'){run=false;pp.textContent='Play';k=Math.max(0,k-1);draw();}
 else if(e.key=='z'||e.key=='Z')zm.onclick();else if(e.key=='x'||e.key=='X')$('sw').onclick();
 else if(e.key=='a'||e.key=='A')$('au').onclick();
 else if(e.key=='1')setMode('f');else if(e.key=='2')setMode('h');else if(e.key=='3')setMode('s');};
window.onresize=layout;changed();requestAnimationFrame(tick);
</script></body></html>
)JS";
    wchar_t page[MAX_PATH]; swprintf_s(page, L"%s_benchmark.html", base);
    bool pageOk = false;
    if (FILE* f = _wfopen(page, L"wb")) { fwrite(s.data(), 1, s.size(), f); fclose(f); pageOk = true; }
    char p8[MAX_PATH * 3]; BrUtf8(page, p8, sizeof(p8));
    Log("bench: report %s (%zu checkpoints), csv %s, page %s", p8, pts.size(), csvOk ? "ok" : "FAILED", pageOk ? "ok" : "FAILED");
    return csvOk && pageOk;
}
