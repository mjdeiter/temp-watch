#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <array>
#include <iomanip>
#include <functional>
#include <cmath>
#include <unistd.h>

static std::string exec(const std::string& cmd) {
    std::array<char, 4096> buf;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    while (fgets(buf.data(), buf.size(), pipe.get()))
        result += buf.data();
    return result;
}

namespace C {
    const char* reset  = "\033[0m";
    const char* bold   = "\033[1m";
    const char* red    = "\033[31m";
    const char* yellow = "\033[33m";
    const char* green  = "\033[32m";
    const char* cyan   = "\033[36m";
    const char* grey   = "\033[90m";
}

static std::string colour(const std::string& s, const char* col) {
    return std::string(col) + s + C::reset;
}

struct Process {
    int    pid;
    std::string user;
    double cpu;
    double mem;
    std::string cmd;
    std::string full;
};

static std::string shortName(const std::string& full) {
    std::istringstream ss(full);
    std::string tok;
    ss >> tok;
    auto sl = tok.rfind('/');
    return (sl != std::string::npos) ? tok.substr(sl + 1) : tok;
}

static std::vector<Process> topProcesses(int n = 15) {
    std::string out = exec("ps -eo pid,%cpu,%mem,user,cmd --sort=-%cpu --no-headers 2>/dev/null | head -" + std::to_string(n + 5));
    std::vector<Process> procs;
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line) && (int)procs.size() < n) {
        std::istringstream ls(line);
        Process p;
        std::string cpu_s, mem_s;
        if (!(ls >> p.pid >> cpu_s >> mem_s >> p.user)) continue;
        p.cpu = std::stod(cpu_s);
        p.mem = std::stod(mem_s);
        std::getline(ls >> std::ws, p.full);
        p.cmd = shortName(p.full);
        if (p.full.size() > 80) p.full = p.full.substr(0, 77) + "...";
        procs.push_back(p);
    }
    return procs;
}

struct ThermalInfo {
    double tctl    = -1;
    double edge    = -1;
    double fan_rpm = -1;
    double nvme    = -1;
};

static double extractJsonDouble(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return -1;
    auto colon = json.find(':', pos);
    if (colon == std::string::npos) return -1;
    auto vstart = json.find_first_not_of(" \t\r\n", colon + 1);
    if (vstart == std::string::npos) return -1;
    try { return std::stod(json.substr(vstart)); } catch (...) { return -1; }
}

static ThermalInfo readThermal() {
    ThermalInfo ti;
    std::string j = exec("sensors -j 2>/dev/null");
    if (j.empty()) return ti;
    { auto pos = j.find("\"Tctl\""); if (pos != std::string::npos) ti.tctl = extractJsonDouble(j.substr(pos, 200), "temp1_input"); }
    { auto pos = j.find("\"edge\""); if (pos != std::string::npos) ti.edge = extractJsonDouble(j.substr(pos, 200), "temp1_input"); }
    { auto pos = j.find("\"fan1\""); if (pos != std::string::npos) ti.fan_rpm = extractJsonDouble(j.substr(pos, 200), "fan1_input"); }
    { auto pos = j.find("\"Composite\""); if (pos != std::string::npos) ti.nvme = extractJsonDouble(j.substr(pos, 200), "temp1_input"); }
    return ti;
}

static const char* tempColour(double t) { return t >= 90 ? C::red : t >= 75 ? C::yellow : C::green; }
static const char* fanColour(double r)  { return r >= 4000 ? C::red : r >= 3000 ? C::yellow : C::green; }
static const char* cpuColour(double p)  { return p >= 50 ? C::red : p >= 20 ? C::yellow : C::green; }

struct Finding { std::string tag, detail; int severity; };

static std::vector<Finding> diagnose(const ThermalInfo& ti, const std::vector<Process>& procs) {
    std::vector<Finding> findings;
    if (ti.tctl >= 90) findings.push_back({"CRIT", "CPU Tctl at " + std::to_string((int)ti.tctl) + " C -- thermal throttling likely", 2});
    else if (ti.tctl >= 75) findings.push_back({"WARN", "CPU Tctl at " + std::to_string((int)ti.tctl) + " C -- elevated, fans working hard", 1});
    if (ti.edge >= 85) findings.push_back({"CRIT", "iGPU edge at " + std::to_string((int)ti.edge) + " C", 2});
    else if (ti.edge >= 70) findings.push_back({"WARN", "iGPU edge at " + std::to_string((int)ti.edge) + " C -- elevated", 1});
    if (ti.nvme >= 70) findings.push_back({"WARN", "NVMe at " + std::to_string((int)ti.nvme) + " C", 1});
    if (ti.fan_rpm >= 4500) findings.push_back({"CRIT", "Fan at " + std::to_string((int)ti.fan_rpm) + " RPM -- maxed out", 2});
    else if (ti.fan_rpm >= 3500) findings.push_back({"WARN", "Fan at " + std::to_string((int)ti.fan_rpm) + " RPM -- running hard", 1});
    double total_cpu = 0;
    for (auto& p : procs) total_cpu += p.cpu;
    for (auto& p : procs) {
        if (p.cpu >= 80) findings.push_back({"CRIT", p.cmd + " (PID " + std::to_string(p.pid) + ") using " + std::to_string((int)p.cpu) + "% CPU", 2});
        else if (p.cpu >= 30) findings.push_back({"WARN", p.cmd + " (PID " + std::to_string(p.pid) + ") using " + std::to_string((int)p.cpu) + "% CPU", 1});
    }
    if (total_cpu >= 200) findings.push_back({"WARN", "Total CPU load is " + std::to_string((int)total_cpu) + "% across all cores", 1});
    if (findings.empty()) findings.push_back({"OK", "Nothing obviously wrong -- fan curve may just be reacting to a recent spike", 0});
    return findings;
}

static void printHeader() {
    std::cout << "\n" << colour("╔══════════════════════════════════════════════╗", C::cyan) << "\n";
    std::cout <<         colour("║          FAN-WATCH  --  Heat Investigator    ║", C::cyan) << "\n";
    std::cout <<         colour("╚══════════════════════════════════════════════╝", C::cyan) << "\n\n";
}

static void printThermal(const ThermalInfo& ti) {
    std::cout << colour("-- Thermals & Fan -----------------------------\n", C::bold);
    auto fmtTemp = [](double t, const char* label) {
        if (t < 0) { std::cout << "  " << label << ": N/A\n"; return; }
        std::ostringstream ss; ss << std::fixed << std::setprecision(1) << t;
        std::cout << "  " << std::setw(10) << std::left << label << colour(ss.str() + " C", tempColour(t)) << "\n";
    };
    fmtTemp(ti.tctl, "CPU Tctl");
    fmtTemp(ti.edge, "iGPU edge");
    fmtTemp(ti.nvme, "NVMe");
    if (ti.fan_rpm >= 0)
        std::cout << "  " << std::setw(10) << std::left << "Fan" << colour(std::to_string((int)ti.fan_rpm) + " RPM", fanColour(ti.fan_rpm)) << "\n";
    std::cout << "\n";
}

static void printProcesses(const std::vector<Process>& procs) {
    std::cout << colour("-- Top CPU Consumers ---------------------------\n", C::bold);
    std::cout << colour("  PID      USER         %CPU  %MEM  NAME\n", C::grey);
    int shown = 0;
    for (auto& p : procs) {
        if (p.cpu < 0.5 && shown >= 10) break;
        std::ostringstream cpu_s, mem_s;
        cpu_s << std::fixed << std::setprecision(1) << p.cpu;
        mem_s << std::fixed << std::setprecision(1) << p.mem;
        std::cout << "  " << std::setw(7) << std::left << p.pid
                  << std::setw(13) << p.user.substr(0, 12)
                  << colour(cpu_s.str() + "%", cpuColour(p.cpu))
                  << "  " << std::setw(5) << mem_s.str() + "%"
                  << "  " << colour(p.cmd, C::bold) << "\n";
        ++shown;
    }
    std::cout << "\n";
}

static void printFindings(const std::vector<Finding>& findings) {
    std::cout << colour("-- Diagnosis -----------------------------------\n", C::bold);
    for (auto& f : findings) {
        const char* col = (f.severity == 2) ? C::red : (f.severity == 1) ? C::yellow : C::green;
        std::string tag = "[" + f.tag + "]";
        std::cout << "  " << colour(std::string(6 - tag.size(), ' ') + tag, col) << "  " << f.detail << "\n";
    }
    std::cout << "\n";
}

static void printSuggestions(const std::vector<Process>& procs, const ThermalInfo& ti) {
    std::cout << colour("-- Suggested Actions ---------------------------\n", C::bold);
    bool any = false;
    for (auto& p : procs) {
        if (p.cpu >= 80) { std::cout << "  -> Kill runaway:  kill -9 " << p.pid << "  (" << colour(p.cmd, C::bold) << ")\n"; any = true; }
        else if (p.cpu >= 30) { std::cout << "  -> Consider stopping:  kill " << p.pid << "  (" << colour(p.cmd, C::bold) << ")\n"; any = true; }
    }
    for (auto& p : procs) {
        if (p.cpu >= 5) {
            if (p.cmd == "parsecd") { std::cout << "  -> Parsec eating CPU. If not in use: pkill parsecd\n"; any = true; }
            if (p.cmd == "anydesk" || p.full.find("crash-handler") != std::string::npos) { std::cout << "  -> AnyDesk crash handler: pkill -f \"anydesk --crash-handler\"\n"; any = true; }
        }
    }
    if (ti.tctl >= 75) { std::cout << "  -> Check thermal paste / vents if temps stay high at idle\n"; any = true; }
    if (!any) std::cout << "  -> Nothing urgent. Monitor for a minute to see if it settles.\n";
    std::cout << "\n";
}

int main(int argc, char** argv) {
    bool watch = false;
    int interval = 5;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--watch" || a == "-w") watch = true;
        if ((a == "--interval" || a == "-i") && i + 1 < argc) interval = std::stoi(argv[++i]);
        if (a == "--help" || a == "-h") {
            std::cout << "Usage: fan-watch [--watch] [--interval N]\n"
                      << "  (no flags)  single snapshot + diagnosis\n"
                      << "  --watch/-w  refresh every N seconds (default 5)\n"
                      << "  --interval  set refresh interval\n";
            return 0;
        }
    }
    auto run = [&]() {
        if (watch) std::cout << "\033[2J\033[H";
        printHeader();
        ThermalInfo ti = readThermal();
        auto procs = topProcesses(12);
        printThermal(ti);
        printProcesses(procs);
        printFindings(diagnose(ti, procs));
        printSuggestions(procs, ti);
    };
    if (!watch) { run(); return 0; }
    std::cout << colour("Watch mode -- refreshing every " + std::to_string(interval) + "s. Ctrl+C to quit.\n", C::grey);
    while (true) { run(); sleep(interval); }
}
