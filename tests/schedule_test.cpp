#include <cassert>
#include <iostream>
#include "schedule.h"

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
    std::cout << "Firmware validation and interpolation: PASS\n";
}
