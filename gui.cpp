#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_glfw.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <fstream>
#include <unistd.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <iomanip>
#include <sys/stat.h>


// ─── version & changelog ────────────────────────────────────────────────────
static const char* APP_VERSION = "1.6.0";
#ifndef BUILD_HASH
#define BUILD_HASH "dev"
#endif
static const char* BUILD_HASH_STR = BUILD_HASH;

struct ChangeEntry { const char* version; const char* date; const char* notes; };
static const ChangeEntry CHANGELOG[] = {
    { "1.6.0", "2026-09-12",
      "- Process list: now scrollable (fixed-height child window, mouse wheel + scrollbar)\n"
      "- Process list: fetches all running processes, not just top 14\n"
      "- Process list: removed 0.3% CPU cutoff so idle processes are visible too" },
    { "1.5.0", "2026-04-24",
      "- AI: fallback chain corrected: gemini-2.5-flash -> gemini-2.5-flash-lite\n"
      "- AI: git hash embedded in About window (build.sh)\n"
      "- Build: build.sh bakes git short hash at compile time\n"
      "- Build: git repo initialized, releases tagged" },
    { "1.4.0", "2026-04-24",
      "- AI: removed thinkingConfig (caused 400 on every request)\n"
      "- AI: non-streaming generateContent; error display in red\n"
      "- AI: model fallback with retry; animated spinner\n"
      "- Process list: removed mini bar clipping names" },
    { "1.3.0", "2025-04-20",
      "- AI: fixed gemini-2.5-flash token starvation (thinkingBudget=0, 2048 tok)\n"
      "- AI: word-wrap display (BeginChild + PushTextWrapPos)\n"
      "- AI: Copy response button; prompt now includes load avg + memory\n"
      "- AI: always sends top 10 procs regardless of CPU%\n"
      "- PID Inspector: selectable text + Copy button" },
    { "1.2.0", "2025-04-19",
      "- Thermometer taskbar icon (procedural 32x32, no external file)\n"
      "- Diagnostic Log window with copy-all and clear\n"
      "- Background refresh thread with configurable interval\n"
      "- Sparkline history graphs (2-min ring buffer)" },
    { "1.1.0", "2025-04-19",
      "- AI Diagnosis panel powered by Gemini 2.5 Flash (SSE streaming)\n"
      "- Run Command panel for ad-hoc shell commands\n"
      "- PID Inspector: look up any PID on demand" },
    { "1.0.0", "2025-04-19",
      "- Initial ImGui release: thermals, fan, top CPU consumers\n"
      "- Colour-coded gauges (green/amber/red thresholds)\n"
      "- Replaced terminal fan-watch with full GUI" },
};
static const int CHANGELOG_COUNT = (int)(sizeof(CHANGELOG)/sizeof(CHANGELOG[0]));


// ─── system identity (read once at startup) ─────────────────────────────────
struct SysInfo { std::string machine, cpu, os; };

static SysInfo readSysInfo(){
    SysInfo si;

    // machine: /sys/class/dmi/id/product_name
    { std::ifstream f("/sys/class/dmi/id/product_name");
      if(f) std::getline(f,si.machine); }
    if(si.machine.empty()) si.machine="Unknown machine";

    // cpu: first "model name" line from /proc/cpuinfo
    { std::ifstream f("/proc/cpuinfo"); std::string line;
      while(std::getline(f,line)){
          if(line.rfind("model name",0)==0){
              auto c=line.find(':');
              if(c!=std::string::npos){
                  si.cpu=line.substr(c+2);
                  // strip trailing " with Radeon Graphics" to keep it compact
                  auto trim=si.cpu.find(" with Radeon");
                  if(trim!=std::string::npos) si.cpu=si.cpu.substr(0,trim);
              }
              break;
          }
      }
    }
    if(si.cpu.empty()) si.cpu="Unknown CPU";

    // os: PRETTY_NAME from /etc/os-release
    { std::ifstream f("/etc/os-release"); std::string line;
      while(std::getline(f,line)){
          if(line.rfind("PRETTY_NAME=",0)==0){
              si.os=line.substr(12);
              // strip surrounding quotes if present
              if(si.os.size()>=2 && si.os.front()=='"') si.os=si.os.substr(1,si.os.size()-2);
              break;
          }
      }
    }
    if(si.os.empty()) si.os="Unknown OS";

    return si;
}
static SysInfo gSysInfo;   // populated in main() before the loop


// ─── hardware inventory (read once at startup) ───────────────────────────────
struct HWComponent { std::string category, vendor, model, detail; };

static std::vector<HWComponent> readHardwareInfo(){
    std::vector<HWComponent> hw;
    auto trim=[](std::string s)->std::string{
        size_t a=s.find_first_not_of(" \t\r\n");
        size_t b=s.find_last_not_of(" \t\r\n");
        return (a==std::string::npos)?"":s.substr(a,b-a+1);
    };
    auto readFile=[&](const std::string& path)->std::string{
        std::ifstream f(path); std::string s;
        if(f) std::getline(f,s); return trim(s);
    };

    // ── System board ─────────────────────────────────────────────────
    {
        std::string vendor  = readFile("/sys/class/dmi/id/sys_vendor");
        std::string product = readFile("/sys/class/dmi/id/product_name");
        std::string version = readFile("/sys/class/dmi/id/product_version");
        std::string board   = readFile("/sys/class/dmi/id/board_name");
        hw.push_back({"System",    vendor, product, "Board: "+board+"  Version: "+version});
    }

    // ── BIOS ─────────────────────────────────────────────────────────
    {
        std::string vendor  = readFile("/sys/class/dmi/id/bios_vendor");
        std::string version = readFile("/sys/class/dmi/id/bios_version");
        std::string date    = readFile("/sys/class/dmi/id/bios_date");
        hw.push_back({"BIOS", vendor, version, "Date: "+date});
    }

    // ── CPU ──────────────────────────────────────────────────────────
    {
        std::string vendor, model, cores, threads, cache;
        std::ifstream f("/proc/cpuinfo"); std::string line;
        while(std::getline(f,line)){
            auto kv=[&](const std::string& key)->std::string{
                if(line.rfind(key,0)!=0) return "";
                auto c=line.find(':'); return c==std::string::npos?"":trim(line.substr(c+1));
            };
            if(auto v=kv("vendor_id");   !v.empty()) vendor =v;
            if(auto v=kv("model name");  !v.empty()) model  =v;
            if(auto v=kv("cpu cores");   !v.empty()) cores  =v;
            if(auto v=kv("siblings");    !v.empty()) threads=v;
            if(auto v=kv("cache size");  !v.empty()) cache  =v;
        }
        if(vendor=="AuthenticAMD") vendor="AMD";
        else if(vendor=="GenuineIntel") vendor="Intel";
        hw.push_back({"CPU", vendor, model, cores+" cores / "+threads+" threads   L2: "+cache});
    }

    // ── RAM slots ────────────────────────────────────────────────────
    {
        // parse dmidecode -t memory without root by reading cached sysfs where possible
        // fall back to /proc/meminfo total if dmidecode unavailable
        std::string dmi_out;
        {
            std::array<char,4096> buf;
            std::unique_ptr<FILE,decltype(&pclose)> p(popen("dmidecode -t memory 2>/dev/null","r"),pclose);
            if(p) while(fgets(buf.data(),buf.size(),p.get())) dmi_out+=buf.data();
        }
        if(!dmi_out.empty()){
            // split by "Memory Device" blocks
            std::istringstream ss(dmi_out); std::string line;
            std::string mfr,part,size,speed,slot,type;
            bool inDevice=false;
            auto flush=[&](){
                if(!size.empty()&&size!="No Module Installed"&&size!="Unknown"){
                    hw.push_back({"RAM", mfr, size+" "+type+" @ "+speed, "Slot: "+slot+"   Part: "+part});
                }
                mfr=part=size=speed=slot=type="";
            };
            auto val=[&](const std::string& l, const std::string& key)->std::string{
                auto p=l.find(key+": "); if(p==std::string::npos) return "";
                return trim(l.substr(p+key.size()+2));
            };
            while(std::getline(ss,line)){
                if(line.find("Memory Device")==0&&line.find("Array")==std::string::npos){
                    if(inDevice) flush(); inDevice=true; continue;
                }
                if(!inDevice) continue;
                if(auto v=val(line,"Manufacturer");!v.empty()) mfr=v;
                if(auto v=val(line,"Part Number");  !v.empty()) part=v;
                if(auto v=val(line,"Size");         !v.empty()) size=v;
                if(auto v=val(line,"Speed");        !v.empty()&&speed.empty()) speed=v;
                if(auto v=val(line,"Locator");      !v.empty()&&slot.empty()) slot=v;
                if(auto v=val(line,"Type");         !v.empty()&&type.empty()&&v!="Unknown"&&v!="Error Correcting Codes (ECC)") type=v;
            }
            if(inDevice) flush();
        } else {
            // fallback: total from /proc/meminfo
            std::ifstream mf("/proc/meminfo"); std::string l;
            while(std::getline(mf,l)){
                if(l.rfind("MemTotal",0)==0){
                    auto c=l.find(':'); std::string kb=trim(l.substr(c+1));
                    hw.push_back({"RAM","","~"+kb,"(dmidecode not available)"});
                    break;
                }
            }
        }
    }

    // ── Storage ──────────────────────────────────────────────────────
    {
        std::array<char,4096> buf; std::string out;
        std::unique_ptr<FILE,decltype(&pclose)> p(
            popen("lsblk -dn -o NAME,MODEL,VENDOR,SIZE,TRAN 2>/dev/null","r"),pclose);
        if(p) while(fgets(buf.data(),buf.size(),p.get())) out+=buf.data();
        std::istringstream ss(out); std::string line;
        while(std::getline(ss,line)){
            if(line.empty()) continue;
            std::istringstream ls(line);
            std::string name,model,vendor,size,tran;
            ls>>name>>model;
            // remaining tokens
            std::vector<std::string> rest; std::string tok;
            while(ls>>tok) rest.push_back(tok);
            if(rest.size()>=2){ size=rest[rest.size()-2]; tran=rest.back(); }
            else if(rest.size()==1){ size=rest[0]; }
            if(model.empty()||model==name) continue;
            // trim trailing spaces from model
            model=trim(model);
            hw.push_back({"Storage", "", model, size+"   interface: "+tran+"   device: "+name});
        }
    }

    // ── GPU ──────────────────────────────────────────────────────────
    {
        std::array<char,4096> buf; std::string out;
        std::unique_ptr<FILE,decltype(&pclose)> p(
            popen("lspci 2>/dev/null | grep -i 'vga\\|3d\\|display'","r"),pclose);
        if(p) while(fgets(buf.data(),buf.size(),p.get())) out+=buf.data();
        std::istringstream ss(out); std::string line;
        while(std::getline(ss,line)){
            if(line.empty()) continue;
            // strip PCI address prefix
            auto c=line.find(": "); std::string rest=(c!=std::string::npos)?line.substr(c+2):line;
            // class
            auto cl=rest.find(": "); std::string cls=(cl!=std::string::npos)?trim(rest.substr(0,cl)):"";
            std::string model=(cl!=std::string::npos)?trim(rest.substr(cl+2)):rest;
            // extract vendor
            std::string vendor;
            if(model.find("Advanced Micro Devices")!=std::string::npos) vendor="AMD";
            else if(model.find("NVIDIA")!=std::string::npos) vendor="NVIDIA";
            else if(model.find("Intel")!=std::string::npos) vendor="Intel";
            hw.push_back({"GPU", vendor, model, cls});
        }
    }

    // ── Display ──────────────────────────────────────────────────────
    {
        std::string mfr,res,size_mm;
        std::array<char,4096> buf; std::string out;
        std::unique_ptr<FILE,decltype(&pclose)> p(
            popen("for f in /sys/class/drm/card*/card*-*/edid; do edid-decode \"$f\" 2>/dev/null; done","r"),pclose);
        if(p) while(fgets(buf.data(),buf.size(),p.get())) out+=buf.data();
        std::istringstream ss(out); std::string line;
        while(std::getline(ss,line)){
            if(line.find("Manufacturer:")!=std::string::npos){
                auto c=line.find(": "); if(c!=std::string::npos) mfr=trim(line.substr(c+2));
            }
            if(line.find("DTD 1:")!=std::string::npos){
                // e.g. "DTD 1:  1920x1080   60.31 Hz  16:9  ... (309 mm x 174 mm)"
                auto px=line.find("x"); 
                auto sp=line.find("  ");
                if(sp!=std::string::npos) res=trim(line.substr(sp));
                // grab mm
                auto mm=line.find("mm x");
                if(mm!=std::string::npos){
                    auto op=line.rfind('(',mm); auto cl=line.find(')',mm);
                    if(op!=std::string::npos&&cl!=std::string::npos)
                        size_mm=line.substr(op+1,cl-op-1);
                }
            }
        }
        if(!res.empty()) hw.push_back({"Display", mfr, trim(res), "Physical: "+size_mm});
    }

    // ── Network ──────────────────────────────────────────────────────
    {
        std::array<char,4096> buf; std::string out;
        std::unique_ptr<FILE,decltype(&pclose)> p(
            popen("lspci 2>/dev/null | grep -i 'network\\|ethernet\\|wireless\\|wi-fi'","r"),pclose);
        if(p) while(fgets(buf.data(),buf.size(),p.get())) out+=buf.data();
        // also lsusb for USB NICs
        std::unique_ptr<FILE,decltype(&pclose)> p2(
            popen("lsusb 2>/dev/null | grep -i 'ethernet\\|wireless\\|wi-fi\\|wlan'","r"),pclose);
        if(p2) while(fgets(buf.data(),buf.size(),p2.get())) out+=buf.data();
        std::istringstream ss(out); std::string line;
        while(std::getline(ss,line)){
            if(line.empty()) continue;
            auto c=line.find(": "); std::string rest=(c!=std::string::npos)?line.substr(c+2):line;
            auto cl=rest.find(": "); std::string cls=(cl!=std::string::npos)?trim(rest.substr(0,cl)):"";
            std::string model=(cl!=std::string::npos)?trim(rest.substr(cl+2)):rest;
            std::string vendor;
            if(model.find("Realtek")!=std::string::npos) vendor="Realtek";
            else if(model.find("Intel")!=std::string::npos) vendor="Intel";
            else if(model.find("Qualcomm")!=std::string::npos||model.find("Atheros")!=std::string::npos) vendor="Qualcomm";
            else if(model.find("Broadcom")!=std::string::npos) vendor="Broadcom";
            hw.push_back({"Network", vendor, model, cls});
        }
    }

    // ── Battery ──────────────────────────────────────────────────────
    {
        for(int i=0;i<4;i++){
            std::string base="/sys/class/power_supply/BAT"+std::to_string(i);
            std::ifstream test(base+"/present"); if(!test) break;
            std::string mfr     = readFile(base+"/manufacturer");
            std::string model   = readFile(base+"/model_name");
            std::string tech    = readFile(base+"/technology");
            std::string ef_d_s  = readFile(base+"/energy_full_design");
            std::string ef_s    = readFile(base+"/energy_full");
            std::string detail;
            if(!ef_d_s.empty()){
                try{
                    int ef_d=std::stoi(ef_d_s)/1000;
                    int ef  =ef_s.empty()?ef_d:std::stoi(ef_s)/1000;
                    detail=tech+"   Design: "+std::to_string(ef_d)+" mWh   Current full: "+std::to_string(ef)+" mWh";
                } catch(...){ detail=tech; }
            } else { detail=tech; }
            hw.push_back({"Battery", mfr, model.empty()?"BAT"+std::to_string(i):model, detail});
        }
    }

    // ── USB devices (non-hub, interesting) ───────────────────────────
    {
        std::array<char,4096> buf; std::string out;
        std::unique_ptr<FILE,decltype(&pclose)> p(
            popen("lsusb 2>/dev/null | grep -iv 'root hub\\|hub device\\| hub'","r"),pclose);
        if(p) while(fgets(buf.data(),buf.size(),p.get())) out+=buf.data();
        std::istringstream ss(out); std::string line;
        while(std::getline(ss,line)){
            if(line.empty()) continue;
            // "Bus NNN Device NNN: ID xxxx:xxxx Vendor Product"
            auto id=line.find("ID "); auto colon=line.find(": ",id);
            std::string vendor_product=(colon!=std::string::npos)?trim(line.substr(colon+2)):trim(line);
            // skip if already captured as network
            if(vendor_product.find("Ethernet")!=std::string::npos) continue;
            // split vendor from product (first word = vendor)
            std::istringstream vp(vendor_product); std::string vword; std::getline(vp,vword,' ');
            hw.push_back({"USB", vword, vendor_product, ""});
        }
    }

    return hw;
}
static std::vector<HWComponent> gHWInfo;

// ─── shell helper ────────────────────────────────────────────────────────────
static std::string shellExec(const std::string& cmd) {
    std::array<char,4096> buf;
    std::string out;
    std::unique_ptr<FILE,decltype(&pclose)> p(popen(cmd.c_str(),"r"),pclose);
    if(!p) return "";
    while(fgets(buf.data(),buf.size(),p.get())) out+=buf.data();
    return out;
}

// ─── data structs ────────────────────────────────────────────────────────────
struct ProcInfo {
    int pid; std::string user,cmd,full;
    float cpu,mem;
};

struct ThermalSnap {
    float tctl=-1,edge=-1,nvme=-1,fan=-1;
};

struct SystemState {
    ThermalSnap thermal;
    std::vector<ProcInfo> procs;
};

// ─── parsing ─────────────────────────────────────────────────────────────────
static float jsonDouble(const std::string& j, const std::string& key) {
    auto pos=j.find("\""+key+"\""); if(pos==std::string::npos) return -1;
    auto c=j.find(':',pos); if(c==std::string::npos) return -1;
    auto v=j.find_first_not_of(" \t\r\n",c+1); if(v==std::string::npos) return -1;
    try{ return (float)std::stod(j.substr(v)); } catch(...){ return -1; }
}

static ThermalSnap readThermal() {
    ThermalSnap t;
    std::string j=shellExec("sensors -j 2>/dev/null");
    if(j.empty()) return t;
    { auto p=j.find("\"Tctl\"");      if(p!=j.npos) t.tctl=jsonDouble(j.substr(p,200),"temp1_input"); }
    { auto p=j.find("\"edge\"");      if(p!=j.npos) t.edge=jsonDouble(j.substr(p,200),"temp1_input"); }
    { auto p=j.find("\"fan1\"");      if(p!=j.npos) t.fan =jsonDouble(j.substr(p,200),"fan1_input");  }
    { auto p=j.find("\"Composite\""); if(p!=j.npos) t.nvme=jsonDouble(j.substr(p,200),"temp1_input"); }
    return t;
}

static std::string shortName(const std::string& full){
    std::istringstream ss(full); std::string tok; ss>>tok;
    auto sl=tok.rfind('/'); return sl!=tok.npos?tok.substr(sl+1):tok;
}

static std::vector<ProcInfo> readProcs(int n=1000){
    auto out=shellExec("ps -eo pid,%cpu,%mem,user,cmd --sort=-%cpu --no-headers 2>/dev/null | head -"+std::to_string(n+4));
    std::vector<ProcInfo> v;
    std::istringstream ss(out); std::string line;
    while(std::getline(ss,line)&&(int)v.size()<n){
        std::istringstream ls(line); ProcInfo p;
        std::string cs,ms;
        if(!(ls>>p.pid>>cs>>ms>>p.user)) continue;
        try{ p.cpu=(float)std::stod(cs); p.mem=(float)std::stod(ms); }catch(...){ continue; }
        std::getline(ls>>std::ws,p.full);
        p.cmd=shortName(p.full);
        if(p.full.size()>70) p.full=p.full.substr(0,67)+"...";
        v.push_back(p);
    }
    return v;
}

static SystemState buildState(){
    SystemState s;
    s.thermal=readThermal();
    s.procs=readProcs();
    return s;
}

// ─── history ring ────────────────────────────────────────────────────────────
static const int HIST=120;
static std::deque<float> hTctl,hEdge,hFan;
static void pushHistory(const ThermalSnap& t){
    auto push=[](std::deque<float>& d,float v){ d.push_back(v); if((int)d.size()>HIST) d.pop_front(); };
    push(hTctl,t.tctl); push(hEdge,t.edge); push(hFan,t.fan);
}

// ─── AI diagnosis ────────────────────────────────────────────────────────────
static std::string gApiKey;

static std::string loadApiKey(){
    std::ifstream f(std::string(getenv("HOME"))+"/.config/gemini_api_key");
    if(!f) return "";
    std::string k; std::getline(f,k);
    while(!k.empty()&&(k.back()=='\n'||k.back()=='\r'||k.back()==' ')) k.pop_back();
    return k;
}


static std::string getSystemExtra(){
    std::string out;
    std::ifstream la("/proc/loadavg");
    if(la){ std::string s; std::getline(la,s);
        size_t p=0; for(int i=0;i<3;i++){ p=s.find(' ',p+1); if(p==std::string::npos) break; }
        out+="Load avg (1/5/15): "+s.substr(0,p)+"\n"; }
    std::string mem=shellExec("free -h --si 2>/dev/null | grep ^Mem:");
    while(!mem.empty()&&(mem.back()=='\n'||mem.back()=='\r')) mem.pop_back();
    if(!mem.empty()) out+="Memory: "+mem+"\n";
    return out;
}

// Build a plain-text system snapshot for the AI prompt
static std::string buildPrompt(const SystemState& s){
    std::ostringstream o;
    o<<"You are a Linux system performance analyst.\n"
     <<"Live snapshot from an HP EliteBook 645 G10 (AMD Ryzen 7 PRO 7730U, CachyOS Linux).\n\n"
     <<"Be specific to THIS snapshot -- not generic boilerplate.\n"
     <<"1. In 2-4 sentences: explain what is driving the current temperature/fan behavior.\n"
     <<"2. Give 2-3 concrete bullet-point actions the user can take RIGHT NOW.\n"
     <<"If the system is idle and cool, say so briefly and give one proactive tip.\n\n"
     <<"=== THERMALS ===\n";
    if(s.thermal.tctl>=0) o<<"CPU Tctl: "<<(int)s.thermal.tctl<<" C\n";
    if(s.thermal.edge>=0) o<<"iGPU edge: "<<(int)s.thermal.edge<<" C\n";
    if(s.thermal.nvme>=0) o<<"NVMe: "<<(int)s.thermal.nvme<<" C\n";
    if(s.thermal.fan >=0) o<<"Fan: "<<(int)s.thermal.fan<<" RPM\n";
    o<<"\n=== SYSTEM ===\n";
    o<<getSystemExtra();
    o<<"\n=== TOP PROCESSES (by CPU%) ===\n";
    o<<std::left<<std::setw(8)<<"PID"<<std::setw(12)<<"USER"<<std::setw(7)<<"%CPU"<<std::setw(7)<<"%MEM"<<"NAME\n";
    int shown=0;
    for(auto& p:s.procs){
        o<<std::setw(8)<<p.pid<<std::setw(12)<<p.user
         <<std::setw(7)<<std::fixed<<std::setprecision(1)<<p.cpu
         <<std::setw(7)<<std::fixed<<std::setprecision(1)<<p.mem<<p.cmd<<"\n";
        if(++shown>=10) break;
    }
    return o.str();
}

// escape a string for JSON
static std::string jsonEscape(const std::string& s){
    std::string out; out.reserve(s.size()+32);
    for(char c:s){
        if(c=='"')  out+="\\\"";
        else if(c=='\\') out+="\\\\";
        else if(c=='\n') out+="\\n";
        else if(c=='\r') out+="\\r";
        else if(c=='\t') out+="\\t";
        else out+=c;
    }
    return out;
}

// Extract text content from a non-streaming Anthropic response JSON
// Looks for "text":"..." fields
static std::string extractTextFromJson(const std::string& json){
    std::string out;
    size_t pos=0;
    while(true){
        auto t=json.find("\"text\":",pos);
        if(t==json.npos) break;
        auto q=json.find('"',t+7);
        if(q==json.npos) break;
        // collect until unescaped closing quote
        std::string val;
        size_t i=q+1;
        while(i<json.size()){
            if(json[i]=='\\'&&i+1<json.size()){
                char nc=json[i+1];
                if(nc=='"') val+='"';
                else if(nc=='\\') val+='\\';
                else if(nc=='n') val+='\n';
                else if(nc=='r') val+='\r';
                else if(nc=='t') val+='\t';
                else val+=nc;
                i+=2;
            } else if(json[i]=='"') { break; }
            else { val+=json[i++]; }
        }
        out+=val;
        pos=i+1;
    }
    return out;
}

struct AiState {
    std::atomic<bool> running{false};
    std::mutex        mtx;
    std::string       text;      // accumulated response
    bool              done=false;
    bool              error=false;
    std::string       errMsg;
};
static AiState gAi;

static void runAiQuery(const SystemState snap){
    if(gApiKey.empty()){
        std::lock_guard<std::mutex> lk(gAi.mtx);
        gAi.text="No API key found.\nPlace your Gemini key in ~/.config/gemini_api_key";
        gAi.done=true; gAi.error=true; gAi.running=false; return;
    }

    std::string prompt=buildPrompt(snap);

    // non-streaming generateContent — simpler and more reliable than SSE
    // NOTE: thinkingConfig is NOT included — that field is rejected by the API
    std::string body=
        "{\"contents\":[{\"parts\":[{\"text\":\""
        +jsonEscape(prompt)
        +"\"}]}],"
        "\"generationConfig\":{\"maxOutputTokens\":2048,\"temperature\":0.4}}";

    std::string pidStr=std::to_string(getpid());
    std::string tmpBody="/tmp/tw_gem_body_"+pidStr+".json";
    std::string tmpResp="/tmp/tw_gem_resp_"+pidStr+".json";
    std::string tmpErr ="/tmp/tw_gem_err_" +pidStr+".txt";
    { std::ofstream f(tmpBody); f<<body; }

    // helper: extract "message" string from Gemini error JSON
    auto extractErrMsg=[](const std::string& j)->std::string{
        auto mp=j.find("\"message\":");
        if(mp==j.npos) return "";
        auto q1=j.find('"',mp+10); if(q1==j.npos) return "";
        std::string out; size_t i=q1+1;
        while(i<j.size()){
            if(j[i]=='\\'&&i+1<j.size()){ out+=j[i+1]; i+=2; }
            else if(j[i]=='"') break;
            else out+=j[i++];
        }
        return out;
    };

    const int MAX_ATTEMPTS=3;
    for(int attempt=1; attempt<=MAX_ATTEMPTS; ++attempt){

        if(!gAi.running) goto done;   // cancelled

        // update status so the user sees retry progress
        if(attempt>1){
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }

        // build a tiny shell script — cleanest way to avoid quoting nightmares
        // model fallback chain — confirmed working on free-tier key
        static const char* MODELS[]={"gemini-2.5-flash","gemini-2.5-flash-lite"};
        static const int   NMODELS=2;
        // pick model for this attempt (cycle through on repeated 503)
        const char* model=MODELS[std::min(attempt-1,NMODELS-1)];

        // update status to show which model we're trying
        if(attempt>1){
            std::lock_guard<std::mutex> lk(gAi.mtx);
            gAi.text="Retrying with "+std::string(model)+" (attempt "+std::to_string(attempt)+"/"+std::to_string(MAX_ATTEMPTS)+")...";
        }

        std::string tmpSh="/tmp/tw_gem_"+pidStr+".sh";
        {
            std::ofstream f(tmpSh);
            f << "#!/bin/sh\n";
            f << "curl -sS --max-time 45 -X POST \\\n";
            f << "  'https://generativelanguage.googleapis.com/v1beta/models/"
              << model << ":generateContent?key=" << gApiKey << "' \\\n";
            f << "  -H 'Content-Type: application/json' \\\n";
            f << "  --data-binary @" << tmpBody << " \\\n";
            f << "  -o " << tmpResp << " 2>" << tmpErr << "\n";
            f << "echo $?\n";
        }
        chmod(tmpSh.c_str(),0700);

        {
            // run curl, capture exit code
            std::string exitCode;
            std::unique_ptr<FILE,decltype(&pclose)> p(popen(tmpSh.c_str(),"r"),pclose);
            if(p){ char buf[16]={}; if(fgets(buf,sizeof(buf),p.get())) exitCode=buf; }
            unlink(tmpSh.c_str());

            // read response
            std::string resp;
            { std::ifstream rf(tmpResp); if(rf) resp.assign(std::istreambuf_iterator<char>(rf),{}); }
            unlink(tmpResp.c_str());

            // curl failed completely (network, timeout, etc.)
            if(resp.empty()){
                std::string curlErr;
                { std::ifstream ef(tmpErr); if(ef) curlErr.assign(std::istreambuf_iterator<char>(ef),{}); }
                unlink(tmpErr.c_str());
                // trim newline from exitCode
                while(!exitCode.empty()&&(exitCode.back()=='\n'||exitCode.back()=='\r')) exitCode.pop_back();
                std::lock_guard<std::mutex> lk(gAi.mtx);
                gAi.text="curl failed (exit "+exitCode+"):\n"+(curlErr.empty()?"(no stderr)":curlErr.substr(0,400));
                gAi.done=true; gAi.error=true; gAi.running=false;
                goto cleanup;
            }
            unlink(tmpErr.c_str());

            // API returned an error JSON
            if(resp.find("\"error\"")!=std::string::npos){
                // check for 503 — retry if we have attempts left
                if(resp.find("503")!=std::string::npos && attempt<MAX_ATTEMPTS) continue;
                // 429 quota or other hard error — surface to user
                std::string msg=extractErrMsg(resp);
                std::lock_guard<std::mutex> lk(gAi.mtx);
                gAi.text="API Error:\n"+(msg.empty()?resp.substr(0,600):msg);
                gAi.done=true; gAi.error=true; gAi.running=false;
                goto cleanup;
            }

            // success — extract text
            std::string text=extractTextFromJson(resp);
            std::lock_guard<std::mutex> lk(gAi.mtx);
            gAi.text=text.empty()
                ? "Unexpected response (no text field).\n\nRaw (first 600 chars):\n"+resp.substr(0,600)
                : text;
            gAi.done=true; gAi.running=false;
            goto cleanup;
        }
    }

    // all retries exhausted (only 503 path gets here)
    {
        std::lock_guard<std::mutex> lk(gAi.mtx);
        gAi.text="API unavailable after "+std::to_string(MAX_ATTEMPTS)+" attempts (503 UNAVAILABLE).\nGemini free tier is overloaded — try again in a minute.";
        gAi.done=true; gAi.error=true; gAi.running=false;
    }

done:
cleanup:
    unlink(tmpBody.c_str());
}

// ─── log ─────────────────────────────────────────────────────────────────────
struct LogEntry {
    std::string timestamp,aiText;
    ThermalSnap thermal;
};
static std::vector<LogEntry> gDiagLog;
static const int MAX_LOG=100;

static std::string nowStr(){
    time_t t=time(nullptr); char buf[32];
    strftime(buf,sizeof(buf),"%H:%M:%S",localtime(&t)); return buf;
}

static void appendLog(const ThermalSnap& th, const std::string& ai){
    LogEntry e; e.timestamp=nowStr(); e.thermal=th; e.aiText=ai;
    gDiagLog.push_back(e);
    if((int)gDiagLog.size()>MAX_LOG) gDiagLog.erase(gDiagLog.begin());
}

static std::string logToText(){
    std::string out;
    for(auto& e:gDiagLog){
        out+="═══ "+e.timestamp+" ═══\n";
        char tmp[64];
        if(e.thermal.tctl>=0){ snprintf(tmp,sizeof(tmp),"Tctl=%.0fC ",e.thermal.tctl); out+=tmp; }
        if(e.thermal.fan >=0){ snprintf(tmp,sizeof(tmp),"Fan=%.0fRPM",e.thermal.fan);  out+=tmp; }
        out+="\n\n"+e.aiText+"\n\n";
    }
    return out;
}

// ─── background refresh ──────────────────────────────────────────────────────
static std::mutex        gMtx;
static SystemState       gState;
static std::atomic<bool> gDirty{false};
static std::atomic<bool> gRunning{true};

static void refreshThread(int& intervalSec){
    while(gRunning){
        auto s=buildState();
        { std::lock_guard<std::mutex> lk(gMtx); gState=s; pushHistory(s.thermal); gDirty=true; }
        for(int i=0;i<intervalSec*10&&gRunning;i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ─── colour helpers ──────────────────────────────────────────────────────────
static ImVec4 tempCol(float t){ return t>=90?ImVec4{1.f,.2f,.2f,1.f}:t>=75?ImVec4{1.f,.8f,.1f,1.f}:ImVec4{.3f,1.f,.4f,1.f}; }
static ImVec4 fanCol (float r){ return r>=4000?ImVec4{1.f,.2f,.2f,1.f}:r>=3000?ImVec4{1.f,.8f,.1f,1.f}:ImVec4{.3f,1.f,.4f,1.f}; }
static ImVec4 cpuCol (float p){ return p>=50?ImVec4{1.f,.2f,.2f,1.f}:p>=20?ImVec4{1.f,.8f,.1f,1.f}:ImVec4{.3f,1.f,.4f,1.f}; }

// ─── gauge / sparkline ───────────────────────────────────────────────────────
static void Gauge(const char* label,float val,float maxv,ImVec4 col,const char* unit=""){
    char buf[64]; snprintf(buf,sizeof(buf),"%.0f%s",val,unit);
    ImGui::TextUnformatted(label); ImGui::SameLine(110);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,col);
    ImGui::ProgressBar(std::clamp(val/maxv,0.f,1.f),ImVec2(160,18),buf);
    ImGui::PopStyleColor();
}

static void Sparkline(const std::deque<float>& data,float ymin,float ymax,ImVec2 size,ImVec4 col){
    if(data.empty()) return;
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 p=ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p,{p.x+size.x,p.y+size.y},IM_COL32(30,30,30,200));
    dl->AddRect      (p,{p.x+size.x,p.y+size.y},IM_COL32(80,80,80,200));
    float range=ymax-ymin; if(range<=0) range=1;
    ImU32 c=ImGui::ColorConvertFloat4ToU32(col);
    int n=(int)data.size();
    for(int i=1;i<n;i++){
        float x0=p.x+(i-1)/(float)(HIST-1)*size.x, x1=p.x+i/(float)(HIST-1)*size.x;
        float y0=p.y+size.y-(data[i-1]-ymin)/range*size.y;
        float y1=p.y+size.y-(data[i  ]-ymin)/range*size.y;
        dl->AddLine({x0,std::clamp(y0,p.y,p.y+size.y)},{x1,std::clamp(y1,p.y,p.y+size.y)},c,1.5f);
    }
    ImGui::Dummy(size);
}

// ─── window icon (procedural 32x32 thermometer) ──────────────────────────────
static void setAppIcon(GLFWwindow* win){
    const int W=32,H=32;
    static uint8_t px[W*H*4];
    auto S=[&](int x,int y,uint8_t r,uint8_t g,uint8_t b,uint8_t a=255){
        if(x<0||x>=W||y<0||y>=H) return;
        int i=(y*W+x)*4; px[i]=r;px[i+1]=g;px[i+2]=b;px[i+3]=a;
    };
    auto inC=[](float cx,float cy,float px,float py,float r)->bool{
        float dx=px-cx,dy=py-cy; return dx*dx+dy*dy<=r*r;
    };
    const uint8_t BR=14,BG=14,BB=20;
    const uint8_t OtR=175,OtG=182,OtB=198;
    const uint8_t MR=255,MG=70,MB=30;
    const uint8_t ER=18,EG=18,EB=32;
    for(int y=0;y<H;y++) for(int x=0;x<W;x++) S(x,y,BR,BG,BB);
    const float TX=16.f;
    const int   TT=2, TB=21;
    const float OR_=3.5f, IR_=2.0f;
    const int   MERC=11;
    const float BX=16.f,BY=25.5f,BR_=5.2f,BO_=6.3f;
    for(int y=17;y<H;y++) for(int x=8;x<24;x++)
        if(inC(BX,BY,x+.5f,y+.5f,BO_)) S(x,y,OtR,OtG,OtB);
    for(int y=17;y<H;y++) for(int x=8;x<24;x++)
        if(inC(BX,BY,x+.5f,y+.5f,BR_)) S(x,y,MR,MG,MB);
    for(int y=18;y<26;y++) for(int x=9;x<17;x++)
        if(inC(13.f,22.f,x+.5f,y+.5f,2.5f)) S(x,y,255,160,130,150);
    for(int y=TT;y<=TB;y++){
        for(int x=(int)(TX-OR_-0.5f);x<=(int)(TX+OR_+0.5f);x++){
            float dx=x+.5f-TX;
            if(dx<-OR_||dx>OR_) continue;
            if(dx>=-IR_&&dx<=IR_){ if(y>=MERC) S(x,y,MR,MG,MB); else S(x,y,ER,EG,EB); }
            else S(x,y,OtR,OtG,OtB);
        }
    }
    for(int y=TT-3;y<TT+2;y++) for(int x=10;x<23;x++){
        if(inC(TX,TT+0.5f,x+.5f,y+.5f,OR_+0.5f)){
            float dx=x+.5f-TX,dy=y+.5f-TT;
            if(dx*dx+dy*dy<=IR_*IR_) S(x,y,ER,EG,EB); else S(x,y,OtR,OtG,OtB);
        }
    }
    int tx_right=(int)(TX+OR_+2.f);
    for(int i=0;i<3;i++){ int ty=TT+3+i*5; S(tx_right,ty,OtR,OtG,OtB); S(tx_right+1,ty,OtR,OtG,OtB); }
    GLFWimage img; img.width=W; img.height=H; img.pixels=px;
    glfwSetWindowIcon(win,1,&img);
}

// ─── main ────────────────────────────────────────────────────────────────────
int main(){
    gApiKey=loadApiKey();
    gSysInfo=readSysInfo();
    gHWInfo=readHardwareInfo();

    if(!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* win=glfwCreateWindow(900,720,"Matt's Temp Monitor",nullptr,nullptr);
    if(!win){ glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    setAppIcon(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io=ImGui::GetIO();
    io.IniFilename=nullptr;
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style=ImGui::GetStyle();
    style.WindowRounding=6; style.FrameRounding=4; style.GrabRounding=4;
    style.WindowPadding={12,12}; style.ItemSpacing={8,6};
    auto& col=style.Colors;
    col[ImGuiCol_TitleBgActive] ={.10f,.40f,.55f,1.f};
    col[ImGuiCol_Header]        ={.10f,.40f,.55f,.6f};
    col[ImGuiCol_HeaderHovered] ={.10f,.50f,.65f,.8f};
    col[ImGuiCol_Button]        ={.10f,.40f,.55f,.8f};
    col[ImGuiCol_ButtonHovered] ={.10f,.55f,.70f,1.f};
    col[ImGuiCol_FrameBg]       ={.12f,.12f,.12f,1.f};
    col[ImGuiCol_WindowBg]      ={.08f,.08f,.10f,1.f};
    col[ImGuiCol_PlotHistogram] ={.10f,.70f,.40f,1.f};

    ImGui_ImplGlfw_InitForOpenGL(win,true);
    ImGui_ImplOpenGL3_Init("#version 330");

    { auto s=buildState(); std::lock_guard<std::mutex> lk(gMtx); gState=s; pushHistory(s.thermal); }

    int refreshInterval=5;
    std::thread bg(refreshThread,std::ref(refreshInterval));

    SystemState snap;
    char killBuf[128]="";
    bool showLog=false;
    bool showAbout=false;
    bool showHW=false;
    std::string logText;

    // AI panel state
    std::string aiDisplay;      // what we show (refreshed from gAi.text)
    bool        aiPending=false;
    std::thread aiThread;

    // static buffers for PID inspector
    static char pidBuf[16]="";
    static char pidResult[512]="";

    while(!glfwWindowShouldClose(win)){
        glfwPollEvents();
        if(gDirty){ std::lock_guard<std::mutex> lk(gMtx); snap=gState; gDirty=false; }

        // pull AI text
        if(aiPending){
            std::lock_guard<std::mutex> lk(gAi.mtx);
            aiDisplay=gAi.text;
            if(gAi.done){
                aiPending=false;
                if(aiThread.joinable()) aiThread.join();
                // save to log
                appendLog(snap.thermal, aiDisplay);
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int fbw,fbh; glfwGetFramebufferSize(win,&fbw,&fbh);
        ImGui::SetNextWindowPos({0,0});
        ImGui::SetNextWindowSize({(float)fbw,(float)fbh});
        ImGui::Begin("##root",nullptr,
            ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
            ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar);

        // ── title bar ─────────────────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.3f,.9f,1.f,1.f));
        ImGui::SetWindowFontScale(1.35f);
        ImGui::Text("  Matt's Temp Monitor");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();

        ImGui::SameLine(ImGui::GetContentRegionAvail().x-400);
        if(ImGui::Button("Hardware")) showHW=!showHW;
        ImGui::SameLine();
        if(ImGui::Button("About")) showAbout=!showAbout;
        ImGui::SameLine();
        if(ImGui::Button("Diagnostic Log")) showLog=!showLog;
        ImGui::SameLine();
        ImGui::TextDisabled("refresh:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(45);
        ImGui::InputInt("s##ri",&refreshInterval,0);
        refreshInterval=std::clamp(refreshInterval,1,60);
        ImGui::SameLine();
        if(ImGui::Button("Refresh")){
            std::thread([](){ auto s=buildState();
                std::lock_guard<std::mutex> lk(gMtx); gState=s; pushHistory(s.thermal); gDirty=true;
            }).detach();
        }
        ImGui::Separator(); ImGui::Spacing();

        float colW=(ImGui::GetContentRegionAvail().x-8)/2.f;

        // ══ LEFT ══════════════════════════════════════════════════════════
        ImGui::BeginChild("##left",{colW,0},false);

        // Thermals
        if(ImGui::CollapsingHeader("Thermals & Fan",ImGuiTreeNodeFlags_DefaultOpen)){
            auto& t=snap.thermal; ImGui::Spacing();
            if(t.tctl>=0) Gauge("CPU Tctl", t.tctl,105,tempCol(t.tctl)," C");
            if(t.edge>=0) Gauge("iGPU edge",t.edge,105,tempCol(t.edge)," C");
            if(t.nvme>=0) Gauge("NVMe",     t.nvme, 80,tempCol(t.nvme)," C");
            if(t.fan >=0) Gauge("Fan",      t.fan, 6000,fanCol(t.fan)," RPM");
            ImGui::Spacing();
            ImGui::TextDisabled("2-min history  CPU Tctl / iGPU / Fan");
            float sw=(colW-36)/3.f;
            Sparkline(hTctl,30,105,{sw,50},tempCol(snap.thermal.tctl));
            ImGui::SameLine(0,6);
            Sparkline(hEdge,30,105,{sw,50},tempCol(snap.thermal.edge));
            ImGui::SameLine(0,6);
            Sparkline(hFan,0,6000,{sw,50},fanCol(snap.thermal.fan));
            ImGui::Spacing();
        }

        // ── AI Diagnosis ──────────────────────────────────────────────────
        if(ImGui::CollapsingHeader("AI Diagnosis",ImGuiTreeNodeFlags_DefaultOpen)){
            ImGui::Spacing();

            // Ask AI button
            bool busy=aiPending||gAi.running;
            if(busy) ImGui::BeginDisabled();
            if(ImGui::Button(gApiKey.empty()?"No API Key -- set ~/.config/gemini_api_key":"  Ask AI  ")){
                if(!gApiKey.empty()){
                    // kick off query
                    { std::lock_guard<std::mutex> lk(gAi.mtx); gAi.text=""; gAi.done=false; gAi.error=false; gAi.errMsg=""; }
                    gAi.running=true; aiPending=true; aiDisplay="Thinking...";
                    SystemState snapCopy=snap;
                    if(aiThread.joinable()) aiThread.join();
                    aiThread=std::thread(runAiQuery,snapCopy);
                }
            }
            if(busy) ImGui::EndDisabled();
            if(busy){
                // animated spinner: cycles through frames at ~3 fps
                const char* frames[]={"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
                int fi=(int)(ImGui::GetTime()*10)%10;
                ImGui::SameLine();
                ImGui::TextDisabled("%s querying...", frames[fi]);
            }
            if(!gApiKey.empty()){ ImGui::SameLine(); ImGui::TextDisabled("powered by Gemini 2.5 Flash"); }

            ImGui::Spacing();

            // Response area — word-wrapping child window; Copy button below
            float aiH=std::max(80.f, (float)fbh*0.22f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.06f,.08f,.12f,1.f));
            if(ImGui::BeginChild("##ai",{-1,aiH},true)){
                ImGui::PushTextWrapPos(0.0f);
                if(aiDisplay.empty()){
                    ImGui::TextDisabled("Press 'Ask AI' above to analyse the current snapshot.");
                } else {
                    // check if response is an error (set by gAi.error after done)
                    bool isErr=false;
                    { std::lock_guard<std::mutex> lk(gAi.mtx); isErr=gAi.error&&gAi.done; }
                    if(isErr) ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.f,.4f,.4f,1.f));
                    ImGui::TextUnformatted(aiDisplay.c_str());
                    if(isErr) ImGui::PopStyleColor();
                }
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            {
                bool isErr=false;
                { std::lock_guard<std::mutex> lk(gAi.mtx); isErr=gAi.error&&gAi.done; }
                if(!aiDisplay.empty()&&!aiPending&&!isErr){
                    if(ImGui::Button("Copy response##aicopy"))
                        ImGui::SetClipboardText(aiDisplay.c_str());
                }
            }
            ImGui::Spacing();
        }

        // Suggested Actions / run command
        if(ImGui::CollapsingHeader("Run Command",ImGuiTreeNodeFlags_DefaultOpen)){
            ImGui::Spacing();
            ImGui::TextDisabled("Execute a command directly:");
            ImGui::SetNextItemWidth(colW-110);
            bool enter=ImGui::InputText("##kb",killBuf,sizeof(killBuf),ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if((ImGui::Button("Execute")||enter)&&killBuf[0]){
                shellExec(std::string(killBuf)+" 2>/dev/null &");
                memset(killBuf,0,sizeof(killBuf));
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                std::thread([](){ auto s=buildState();
                    std::lock_guard<std::mutex> lk(gMtx); gState=s; pushHistory(s.thermal); gDirty=true;
                }).detach();
            }
            ImGui::Spacing();
        }

        ImGui::EndChild(); // left

        // ══ RIGHT ═════════════════════════════════════════════════════════
        ImGui::SameLine(0,8);
        ImGui::BeginChild("##right",{colW,0},false);

        // Top CPU consumers
        if(ImGui::CollapsingHeader("Top CPU Consumers",ImGuiTreeNodeFlags_DefaultOpen)){
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.5f,.5f,.5f,1.f));
            ImGui::Text("  %-8s %-11s %5s %5s  NAME","PID","USER","%CPU","%MEM");
            ImGui::PopStyleColor();
            ImGui::Separator();

            ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.04f,.04f,.06f,1.f));
            if(ImGui::BeginChild("##proclist",ImVec2(-1,340),true,ImGuiWindowFlags_AlwaysVerticalScrollbar)){
                for(auto& p:snap.procs){
                    // PID as Selectable — right-click gives "Copy PID" context menu
                    char pidStr[16]; snprintf(pidStr,sizeof(pidStr),"%d",p.pid);
                    char pidPopId[32]; snprintf(pidPopId,sizeof(pidPopId),"##pidpop%d",p.pid);
                    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(.15f,.15f,.20f,1.f));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(.20f,.20f,.30f,1.f));
                    ImGui::Selectable(pidStr, false, ImGuiSelectableFlags_None, ImVec2(72,0));
                    ImGui::PopStyleColor(2);
                    if(ImGui::BeginPopupContextItem(pidPopId)){
                        if(ImGui::MenuItem("Copy PID")) ImGui::SetClipboardText(pidStr);
                        ImGui::EndPopup();
                    }

                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text,cpuCol(p.cpu));
                    ImGui::Text("%-11s %4.1f%% %4.1f%%  %s",
                        p.user.substr(0,10).c_str(), p.cpu, p.mem, p.cmd.c_str());
                    ImGui::PopStyleColor();

                    // (mini bar removed — was truncating process names)
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        // PID Inspector
        if(ImGui::CollapsingHeader("PID Inspector",ImGuiTreeNodeFlags_DefaultOpen)){
            ImGui::Spacing();
            ImGui::TextDisabled("Type or paste a PID and press Enter:");
            ImGui::SetNextItemWidth(130);
            bool doLook=ImGui::InputText("##pi",pidBuf,sizeof(pidBuf),
                ImGuiInputTextFlags_CharsDecimal|ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if(ImGui::Button("Look up")||doLook){
                std::string out=shellExec("ps -p "+std::string(pidBuf)+" -o pid,user,%cpu,%mem,cmd --no-headers 2>/dev/null");
                if(out.empty()) snprintf(pidResult,sizeof(pidResult),"PID not found or already gone");
                else { out.erase(out.find_last_not_of("\n")+1); snprintf(pidResult,sizeof(pidResult),"%s",out.c_str()); }
            }
            if(pidResult[0]){
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.06f,.08f,.12f,1.f));
                if(ImGui::BeginChild("##pr",{-1,62},true)){
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(pidResult);
                    ImGui::PopTextWrapPos();
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
                if(ImGui::Button("Copy##pidcopy"))
                    ImGui::SetClipboardText(pidResult);
            }
            ImGui::Spacing();
        }

        ImGui::EndChild(); // right
        ImGui::End();



        // ── Hardware window ───────────────────────────────────────────────────
        if(showHW){
            ImGui::SetNextWindowSize({680,520},ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos({80,80},ImGuiCond_FirstUseEver);
            ImGui::Begin("Hardware##hwwin",&showHW);

            ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.3f,.9f,1.f,1.f));
            ImGui::TextUnformatted("System Hardware Inventory");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("  %s", gSysInfo.os.c_str());
            ImGui::Separator(); ImGui::Spacing();

            // column headers
            ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.5f,.5f,.5f,1.f));
            ImGui::Text("  %-12s %-18s %-30s %s","CATEGORY","VENDOR / MFR","MODEL","DETAIL");
            ImGui::PopStyleColor();
            ImGui::Separator();

            ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.06f,.07f,.10f,1.f));
            if(ImGui::BeginChild("##hw",{-1,-40},true)){
                std::string lastCat;
                for(auto& c:gHWInfo){
                    // category header when it changes
                    if(c.category!=lastCat){
                        if(!lastCat.empty()) ImGui::Spacing();
                        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.3f,.9f,1.f,.9f));
                        ImGui::TextUnformatted(c.category.c_str());
                        ImGui::PopStyleColor();
                        ImGui::Separator();
                        lastCat=c.category;
                    }
                    // row
                    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.9f,.9f,.9f,1.f));
                    ImGui::Text("  %-18s", c.vendor.substr(0,17).c_str());
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0,0);
                    // model — wrap-aware using TextWrapped in a group
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+360);
                    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.f,1.f,1.f,1.f));
                    ImGui::TextUnformatted(c.model.c_str());
                    ImGui::PopStyleColor();
                    ImGui::PopTextWrapPos();
                    if(!c.detail.empty()){
                        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.55f,.55f,.65f,1.f));
                        ImGui::Text("    %s", c.detail.c_str());
                        ImGui::PopStyleColor();
                    }
                    ImGui::Spacing();
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::Spacing();
            if(ImGui::Button("Copy to clipboard##hwcopy")){
                std::string out;
                for(auto& c:gHWInfo){
                    out+=c.category+"\n";
                    out+="  Vendor:  "+c.vendor+"\n";
                    out+="  Model:   "+c.model+"\n";
                    if(!c.detail.empty()) out+="  Detail:  "+c.detail+"\n";
                    out+="\n";
                }
                ImGui::SetClipboardText(out.c_str());
            }
            ImGui::SameLine();
            if(ImGui::Button("Close##hwclose")) showHW=false;
            ImGui::End();
        }

        // ── About window ──────────────────────────────────────────────────────
        if(showAbout){
            ImGui::SetNextWindowSize({520,420},ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos({160,120},ImGuiCond_FirstUseEver);
            ImGui::Begin("About##aboutwin",&showAbout,ImGuiWindowFlags_NoResize);

            ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.3f,.9f,1.f,1.f));
            ImGui::SetWindowFontScale(1.2f);
            ImGui::Text("Matt's Temp Monitor");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("v%s  (%s)", APP_VERSION, BUILD_HASH_STR);
            ImGui::Spacing();
            ImGui::TextDisabled("%s  |  %s  |  %s", gSysInfo.machine.c_str(), gSysInfo.cpu.c_str(), gSysInfo.os.c_str());
            ImGui::TextDisabled("UI: Dear ImGui + OpenGL3/GLFW   |   AI: Gemini 2.5 Flash (SSE)");
            ImGui::Separator(); ImGui::Spacing();

            ImGui::TextUnformatted("Changelog");
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.06f,.07f,.10f,1.f));
            if(ImGui::BeginChild("##cl",{-1,-40},true)){
                for(int i=0;i<CHANGELOG_COUNT;i++){
                    auto& e=CHANGELOG[i];
                    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.3f,.9f,1.f,1.f));
                    ImGui::Text("v%s", e.version);
                    ImGui::PopStyleColor();
                    ImGui::SameLine(60);
                    ImGui::TextDisabled("%s", e.date);
                    ImGui::Spacing();
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(e.notes);
                    ImGui::PopTextWrapPos();
                    if(i<CHANGELOG_COUNT-1){ ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); }
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::Spacing();
            if(ImGui::Button("Close##aboutclose")) showAbout=false;
            ImGui::End();
        }

        // ── Diagnostic Log window ──────────────────────────────────────────
        if(showLog){
            ImGui::SetNextWindowSize({720,520},ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos({80,100},ImGuiCond_FirstUseEver);
            ImGui::Begin("Diagnostic Log",&showLog);
            if(ImGui::Button("Copy all to clipboard")){
                logText=logToText();
                ImGui::SetClipboardText(logText.c_str());
            }
            ImGui::SameLine();
            if(ImGui::Button("Clear log")) gDiagLog.clear();
            ImGui::SameLine();
            ImGui::TextDisabled("%d entries",(int)gDiagLog.size());
            ImGui::Separator();
            logText=logToText();
            ImGui::InputTextMultiline("##lv",
                const_cast<char*>(logText.c_str()),logText.size()+1,
                {-1,-1},ImGuiInputTextFlags_ReadOnly);
            ImGui::End();
        }

        ImGui::Render();
        glViewport(0,0,fbw,fbh);
        glClearColor(.05f,.05f,.07f,1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    gRunning=false;
    gAi.running=false;
    if(aiThread.joinable()) aiThread.join();
    bg.join();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
