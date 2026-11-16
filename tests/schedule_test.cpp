#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include "schedule.h"
#include "default_schedule.h"

int main() {
    DynamicJsonDocument doc(JSON_OBJECT_SIZE(4) + 4 * (JSON_ARRAY_SIZE(13) + 13 * JSON_OBJECT_SIZE(2)) + 256);
    for (int c=0;c<4;c++) {
        char key[]={'c','h',char('0'+c),0};
        auto a=doc.createNestedArray(key);
        for (auto p : {Point{0,0},Point{60,240},Point{120,0},Point{1440,0}}) {
            auto obj=a.createNestedObject();obj["t"]=p.t;obj["v"]=p.v;
        }
    }
    Schedule candidate;
    auto valid=[&](){ return parseSchedule(doc.as<JsonVariantConst>(),candidate); };
    assert(valid());
    assert(interpolate(candidate.channels[0],0)==0);
    assert(interpolate(candidate.channels[0],1800)==120);
    assert(interpolate(candidate.channels[0],3600)==240);
    assert(interpolate(candidate.channels[0],5400)==120);
    assert(interpolate(candidate.channels[0],7200)==0);
    assert(interpolate(candidate.channels[0],86399)==0);
    assert(trend(candidate.channels[0],0)==1);
    assert(trend(candidate.channels[0],3599)==1);
    assert(trend(candidate.channels[0],3600)==-1);
    assert(trend(candidate.channels[0],7200)==0);
    assert(trend(candidate.channels[0],86399)==0);
    // Smooth curve: exact at the points and the middle, slow near a plateau or a peak.
    assert(interpolateFine(candidate.channels[0],0)==0);
    assert(interpolateFine(candidate.channels[0],1800)==120*FINE_STEPS);
    assert(interpolateFine(candidate.channels[0],3600)==240*FINE_STEPS);
    assert(interpolate(candidate.channels[0],840)>=31 && interpolate(candidate.channels[0],840)<=35);    // 23% of the way: ~14%
    assert(interpolate(candidate.channels[0],2760)>=205 && interpolate(candidate.channels[0],2760)<=209);
    assert(interpolateFine(candidate.channels[0],4440)==interpolateFine(candidate.channels[0],2760));    // mirrored on the way down
    for (uint32_t second=1; second<=3600; ++second)
        assert(interpolateFine(candidate.channels[0],second)>=interpolateFine(candidate.channels[0],second-1));
    for (uint32_t second=3601; second<=7200; ++second)
        assert(interpolateFine(candidate.channels[0],second)<=interpolateFine(candidate.channels[0],second-1));
    assert(interpolateFine(candidate.channels[0],50000)==0);  // flat segment stays flat
    {
        // A point on a continuing rise is passed without stopping or overshooting;
        // the peak that follows is rounded.
        Channel rise; rise.count=5;
        rise.points[0]={0,0}; rise.points[1]={60,100}; rise.points[2]={120,200}; rise.points[3]={180,0}; rise.points[4]={1440,0};
        assert(interpolateFine(rise,3600)==100*FINE_STEPS && interpolateFine(rise,7200)==200*FINE_STEPS);
        int through=interpolateFine(rise,3660)-interpolateFine(rise,3540);   // two minutes around the point
        assert(through>150 && through<280);                                   // a straight line would give 213
        int atPeak=interpolateFine(rise,7200)-interpolateFine(rise,7140);     // last minute before the peak
        assert(atPeak>=0 && atPeak<20);
        for (uint32_t second=1; second<=7200; ++second) {
            assert(interpolateFine(rise,second)>=interpolateFine(rise,second-1));
            assert(interpolateFine(rise,second)<=200*FINE_STEPS);
        }
        // Midnight is an ordinary point: the curve crosses it without a corner.
        Channel night; night.count=4;
        night.points[0]={0,100}; night.points[1]={360,0}; night.points[2]={1080,200}; night.points[3]={1440,100};
        assert(pointTangent(night,0)<0 && pointTangent(night,0)==pointTangent(night,3));
    }
    assert(gammaDuty(0)==0);
    assert(gammaDuty(1)==1);
    assert(gammaDuty(FINE_MAX)==PWM_RANGE);
    assert(gammaDuty(FINE_MAX/2)>200 && gammaDuty(FINE_MAX/2)<240);
    for (uint16_t fine=1; fine<=FINE_MAX; ++fine) assert(gammaDuty(fine)>=gammaDuty(fine-1));
    doc["ch0"][1]["t"]=0;assert(!valid());doc["ch0"][1]["t"]=60;
    doc["ch0"][1]["v"]=256;assert(!valid());
    doc["ch0"][1]["v"]=-1;assert(!valid());
    doc["ch0"][1]["v"]=2.5;assert(!valid());
    doc["ch0"][1]["v"]="240";assert(!valid());doc["ch0"][1]["v"]=240;
    doc["ch0"][3]["v"]=1;assert(!valid());doc["ch0"][3]["v"]=0;
    doc["ch0"][3]["t"]=1439;assert(!valid());doc["ch0"][3]["t"]=1440;
    doc["extra"]=1;assert(!valid());doc.remove("extra");
    doc["ch0"][1]["extra"]=1;assert(!valid());doc["ch0"][1].remove("extra");
    assert(valid());
    // Maximum supported graph fits both JSON memory and transport limits.
    doc.clear();
    for(int c=0;c<4;c++) {
        char key[]={'c','h',char('0'+c),0};auto a=doc.createNestedArray(key);
        for(int i=0;i<12;i++){auto p=a.createNestedObject();p["t"]=i==11?1440:i*120;p["v"]=i==0||i==11?0:255;}
    }
    assert(!doc.overflowed());assert(valid());assert(measureJson(doc)<=MAX_SCHEDULE_BYTES);
    auto p=doc["ch0"].createNestedObject();p["t"]=1440;p["v"]=0;assert(!valid());
    // The schedule built into the firmware is valid and is the one in data/.
    DynamicJsonDocument builtIn(4096);
    assert(!deserializeJson(builtIn, DEFAULT_SCHEDULE_JSON));
    assert(parseSchedule(builtIn.as<JsonVariantConst>(), candidate));
    std::ifstream file("data/schedule.json");
    assert(file.good());   // run from the project root
    std::stringstream content; content << file.rdbuf();
    std::string onDisk = content.str();
    while (!onDisk.empty() && (onDisk.back() == '\n' || onDisk.back() == '\r')) onDisk.pop_back();
    assert(onDisk == DEFAULT_SCHEDULE_JSON);
    std::cout << "Firmware validation and interpolation: PASS\n";
}
