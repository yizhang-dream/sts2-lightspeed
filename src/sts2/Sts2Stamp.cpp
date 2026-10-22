// ContinuationStamp parse/emit implementation. See Sts2Stamp.h for the grammar.

#include "sts2/Sts2Stamp.h"
#include "sts2/Sts2Relic.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace sts2 {

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == sep) { out.push_back(cur); cur.clear(); }
        else cur += ch;
    }
    out.push_back(cur);
    return out;
}

// split a pile field payload into card entries: entries are separated by ','
// but each entry itself contains no ',' (vars use '[..,]' with the comma inside brackets;
// keywords=[] too) — verified against real stamps.
std::vector<std::string> splitCardEntries(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int bracket = 0;
    for (char ch : s) {
        if (ch == '[') ++bracket;
        if (ch == ']') --bracket;
        if (ch == ',' && bracket == 0) { out.push_back(cur); cur.clear(); continue; }
        cur += ch;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

} // namespace

const char* powerName(PowerId id) {
    return powerStampName(id); // generated full-game table (Sts2PowerTable.cpp)
}

PowerId powerIdFromGameName(const std::string& name) {
    return powerIdFromStampName(name.c_str());
}

bool parseRngState(const std::string& s, Sts2Rng& rng, std::string* err) {
    auto p = split(s, ':');
    if (p.size() != 5) { if (err) *err = "rng state expects 5 parts: " + s; return false; }
    Sts2Rng::Serializable ser{};
    ser.counter = std::atoi(p[0].c_str());
    ser.state.s0 = std::strtoull(p[1].c_str(), nullptr, 10);
    ser.state.s1 = std::strtoull(p[2].c_str(), nullptr, 10);
    ser.state.s2 = std::strtoull(p[3].c_str(), nullptr, 10);
    ser.state.s3 = std::strtoull(p[4].c_str(), nullptr, 10);
    rng.LoadFromSerializable(ser);
    return true;
}

std::string emitRngState(const Sts2Rng& rng) {
    auto s = rng.CaptureState();
    char buf[128];
    std::snprintf(buf, sizeof buf, "%d:%llu:%llu:%llu:%llu",
                  s.counter,
                  (unsigned long long)s.state.s0, (unsigned long long)s.state.s1,
                  (unsigned long long)s.state.s2, (unsigned long long)s.state.s3);
    return buf;
}

bool parsePileEntries(const std::string& payload, std::vector<CardInstance>& out, std::string* err) {
    out.clear();
    if (payload.empty()) return true;
    for (const auto& entry : splitCardEntries(payload)) {
        if (entry.empty()) continue;
        auto f = split(entry, '/');
        if (f.size() < 3) { if (err) *err = "bad card entry: " + entry; return false; }
        CardInstance c;
        auto nameUp = split(f[0], '+');
        int idv = lookupCardId(nameUp[0].c_str());
        c.id = (CardId)idv;
        c.upgrade = nameUp.size() > 1 ? std::atoi(nameUp[1].c_str()) : 0;
        c.stampText = entry; // keep the verbatim entry for byte-faithful re-emit
        // IsSlyThisTurn slot (index 5: name, cost, star, replay, exhaust, sly, retain, ...)
        if (f.size() > 3) c.extraReplays = std::atoi(f[3].c_str());
        if (f.size() > 5) c.sly = (f[5] == "True");
        // keyword list: granted-in-combat keywords (Ghost Seed Ethereal, Hex, MasterPlanner)
        // must survive parsing so the replay's turn-end/opinion checks see them.
        for (const auto& seg : f) {
            if (seg.rfind("keywords=[", 0) != 0) continue;
            if (seg.find("Ethereal") != std::string::npos) c.ephemeral = true;
            if (seg.find("Retain") != std::string::npos) c.retain = true;
            if (seg.find("Sly") != std::string::npos) c.sly = true;
            break;
        }
        // Affliction slot (index 10): "-:0[Vars]" or "ENTANGLED:1[Vars]".
        if (f.size() > 10) {
            const std::string& seg = f[10];
            if (seg.rfind("ENTANGLED:", 0) == 0) {
                c.affliction = 1;
                c.afflictionAmount = std::atoi(seg.c_str() + 10);
            } else if (seg.rfind("RINGING:", 0) == 0) {
                c.affliction = 2;
                c.afflictionAmount = std::atoi(seg.c_str() + 8);
            } else if (seg.rfind("TAINTED:", 0) == 0) {
                c.affliction = 3;
                c.afflictionAmount = std::atoi(seg.c_str() + 8);
            } else if (seg.rfind("BOUND:", 0) == 0) {
                c.affliction = 4;
                c.afflictionAmount = std::atoi(seg.c_str() + 6);
            } else if (seg.rfind("HEXED:", 0) == 0) {
                c.affliction = 5;
                c.afflictionAmount = std::atoi(seg.c_str() + 6);
            } else if (seg.rfind("GALVANIZED:", 0) == 0) {
                c.affliction = 6;
                c.afflictionAmount = std::atoi(seg.c_str() + 11);
            }
        }
        // SetToFreeThisTurn leaves a zero-delta modifier (source 6) in the cost-state; the
        // free cost expires at the end of the turn, so remember it for the turn-end clear.
        if (entry.find("0:1:6:False") != std::string::npos) c.parsedFreeCost = true;
        // cost-state (EnergyCost.AddThisCombat ledger): "<base>:<n>[<delta>:2:0:False,...]".
        // The base and the deltas seed energyCost() and later re-emits.
        size_t cs = entry.find("|cost-state=");
        if (cs != std::string::npos) {
            size_t p = cs + 12;
            int base = std::atoi(entry.c_str() + p);
            size_t colon = entry.find(':', p);
            size_t lb = entry.find('[', colon == std::string::npos ? p : colon);
            size_t rb = lb == std::string::npos ? std::string::npos : entry.find(']', lb);
            if (colon != std::string::npos && lb != std::string::npos && rb != std::string::npos) {
                c.costBase = base;
                std::string inner = entry.substr(lb + 1, rb - lb - 1);
                for (auto& tok : split(inner, ','))
                    if (!tok.empty()) c.costMods.push_back(std::atoi(tok.c_str()));
            }
        }
        // In-combat var bumps already present in the stamp (KinglyPunch Damage after a
        // draw, Rampage bumps, ...): seed varBump from stamped - (base + upBy*upgrade).
        {
            const CardDef& cd = c.def();
            for (int i = 0; i < (int)cd.nVars && i < 4; ++i) {
                const char* key = cd.vars[i].key;
                if (!key || !key[0] || std::strncmp(key, "Calculated", 10) == 0) continue;
                std::string pat = std::string(key) + "=";
                size_t vp = entry.find(pat);
                if (vp == std::string::npos) continue;
                int stamped = std::atoi(entry.c_str() + vp + pat.size());
                int expect = cd.vars[i].base + cd.vars[i].upBy * c.upgrade;
                if (stamped != expect) c.varBump[i] = stamped - expect;
            }
        }
        // Wither fake upgrade lives in the Damage var only ([Damage=N,] -> (N-3)/3)
        if (c.id == CardId::WITHER) {
            auto dv = entry.find("[Damage=");
            if (dv != std::string::npos)
                c.fakeUpgrade = std::max(0, (std::atoi(entry.c_str() + dv + 7) - 3) / 3);
        }
        int idx = 0;
        for (auto& inst : out)
            if (inst.id == c.id) ++idx;
        c.uniqueIdx = idx;
        out.push_back(c);
    }
    return true;
}

// Current energy cost carried by the instance (cost-state base + applied deltas).
static int instanceStampCost(const CardInstance& c) {
    if (c.freeThisTurn) return 0;
    const CardDef& d = c.def();
    if (c.costBase < 0 && c.costMods.empty()) {
        if (d.cost < 0) return d.hasFlag(CF_ENERGY_X) ? 0 : -1; // X-cost reads 0 locally
        return d.cost + d.costUp * c.upgrade;
    }
    int base = c.costBase >= 0 ? c.costBase : d.cost + d.costUp * c.upgrade;
    for (int m : c.costMods) base += m;
    return base < 0 ? 0 : base;
}

void applyRetainKeyword(CardInstance& c) {
    if (c.retain) return;
    c.retain = true;
    if (c.stampText.empty()) return;
    auto parts = split(c.stampText, '/');
    if (parts.size() > 6) parts[6] = "True"; // ShouldRetainThisTurn slot
    // the keyword list is emitted in CardKeyword order: Exhaust, Ethereal, Innate,
    // Unplayable, Retain, Sly, Eternal
    for (auto& p : parts) {
        if (p.rfind("keywords=[", 0) != 0) continue;
        std::string inner = p.substr(10, p.size() - 11);
        if (inner.find("Retain") == std::string::npos) {
            static const char* order[] = {"Exhaust", "Ethereal", "Innate", "Unplayable",
                                          "Retain", "Sly", "Eternal"};
            std::vector<std::string> have;
            std::string cur;
            for (char ch : inner) { if (ch == ',') { if (!cur.empty()) have.push_back(cur); cur.clear(); } else cur += ch; }
            if (!cur.empty()) have.push_back(cur);
            have.push_back("Retain");
            std::string rebuilt = "[";
            bool firstKw = true;
            for (const char* k : order)
                for (const auto& h : have)
                    if (h == k) {
                        if (!firstKw) rebuilt += ",";
                        rebuilt += h;
                        firstKw = false;
                    }
            rebuilt += "]";
            p = "keywords=" + rebuilt;
        }
        break;
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) out += '/'; out += parts[i]; }
    c.stampText = out;
}

void applySlyKeyword(CardInstance& c) {
    if (c.sly) return;
    c.sly = true;
    if (c.stampText.empty()) return;
    auto parts = split(c.stampText, '/');
    if (parts.size() > 5) parts[5] = "True"; // IsSlyThisTurn slot
    for (auto& p : parts) {
        if (p.rfind("keywords=[", 0) != 0) continue;
        std::string inner = p.substr(10, p.size() - 11);
        if (inner.find("Sly") != std::string::npos) break;
        static const char* order[] = {"Exhaust", "Ethereal", "Innate", "Unplayable",
                                      "Retain", "Sly", "Eternal"};
        std::vector<std::string> have;
        std::string cur;
        for (char ch : inner) { if (ch == ',') { if (!cur.empty()) have.push_back(cur); cur.clear(); } else cur += ch; }
        if (!cur.empty()) have.push_back(cur);
        have.push_back("Sly");
        std::string rebuilt = "[";
        bool firstKw = true;
        for (const char* k : order)
            for (const auto& h : have)
                if (h == k) {
                    if (!firstKw) rebuilt += ",";
                    rebuilt += h;
                    firstKw = false;
                }
        rebuilt += "]";
        p = "keywords=" + rebuilt;
        break;
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) out += '/'; out += parts[i]; }
    c.stampText = out;
}

void addCostModThisCombat(CardInstance& c, int delta) {
    if (delta == 0) return;
    const CardDef& d = c.def();
    if (c.costBase < 0)
        c.costBase = d.cost < 0 ? d.cost : d.cost + d.costUp * c.upgrade;
    c.costMods.push_back(delta);
    if (c.stampText.empty()) return; // synthesized entries read costBase/costMods directly

    auto parts = split(c.stampText, '/');
    if (parts.size() < 2) return;
    // segment 1 is "<CostsX>:<local cost>"
    size_t colon = parts[1].rfind(':');
    if (colon != std::string::npos)
        parts[1] = parts[1].substr(0, colon + 1) + std::to_string(instanceStampCost(c));

    // cost-state is a sub-field of the private slot; the stars slot follows it
    std::string state = std::to_string(c.costBase) + ":" + std::to_string(c.costMods.size()) + "[";
    for (int m : c.costMods) state += std::to_string(m) + ":2:0:False,";
    state += "]";
    size_t pi = parts.size();
    for (size_t i = 0; i < parts.size(); ++i)
        if (parts[i].rfind("private=", 0) == 0) { pi = i; break; }
    if (pi == parts.size()) return;
    std::string& p = parts[pi];
    size_t bar = p.find("|cost-state=");
    if (bar == std::string::npos) {
        p += "|cost-state=" + state;
        parts.insert(parts.begin() + (long)pi + 1,
                     "stars=" + std::to_string(d.starCost) + ":0[]");
    } else {
        size_t end = p.find('|', bar + 1);
        p = p.substr(0, bar) + "|cost-state=" + state
          + (end == std::string::npos ? std::string() : p.substr(end));
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) out += '/'; out += parts[i]; }
    c.stampText = out;
}

std::string emitCardEntry(const CardInstance& c) {
    if (!c.stampText.empty()) return c.stampText;
    // synthesized entry for engine-created cards (Wither/Dazed generation)
    char buf[64];
    const CardDef& d = c.def();
    std::snprintf(buf, sizeof buf, "%d", instanceStampCost(c));
    std::string vars = "[]";
    if (d.nVars > 0) {
        // The game emits DynamicVars ordered by KEY (StringComparer.Ordinal) and SKIPS any
        // derived CalculatedVar (SemanticStateFieldPolicy: CalculatedVar -> Derived). The
        // engine's "Calculated*" var slots mirror those derived vars, so they are omitted
        // here and the rest are sorted by key to match the stamp byte-for-byte.
        std::vector<std::pair<std::string, int>> kv;
        for (int i = 0; i < (int)d.nVars; ++i) {
            std::string key = d.vars[i].key;
            if (key.rfind("Calculated", 0) == 0) continue; // derived var: not stamped
            int v = cardVarValue(c, d, i);
            if (d.id == CardId::WITHER && i == 0) v += 3 * c.fakeUpgrade;
            kv.emplace_back(key, v);
        }
        std::sort(kv.begin(), kv.end(),
                  [](const std::pair<std::string, int>& a, const std::pair<std::string, int>& b) {
                      return a.first < b.first;
                  });
        vars = "[";
        for (auto& e : kv) vars += e.first + std::string("=") + std::to_string(e.second) + ",";
        vars += "]";
    }
    // combat-generated cards (Wither/Dazed/Burn/Anger clones): deck flag False
    const bool deckFlag = !c.genCard;
    // keyword list in CardKeyword declaration order (stamp: GetKeywordsWithSources(...).Order()
    // over the enum: Exhaust=1, Ethereal=2, Innate=3, Unplayable=4, Retain=5, Sly=6, Eternal=7).
    // Cards created mid-combat still carry their canonical keywords
    // (corpus INFINITE_BLADES T2/T3: SHIV+0 shows keywords=[Exhaust]).
    std::string keywords = "[";
    {
        const char* kws[7] = {nullptr};
        if (cardHasFlag(c, CF_EXHAUST)) kws[0] = "Exhaust";
        if (cardHasFlag(c, CF_ETHEREAL) || c.ephemeral) kws[1] = "Ethereal";
        if (cardHasFlag(c, CF_INNATE)) kws[2] = "Innate";
        if (cardHasFlag(c, CF_UNPLAYABLE)) kws[3] = "Unplayable";
        if (cardHasFlag(c, CF_RETAIN)) kws[4] = "Retain";
        if (cardHasFlag(c, CF_SLY) || c.sly) kws[5] = "Sly";
        if (cardHasFlag(c, CF_ETERNAL)) kws[6] = "Eternal";
        bool first = true;
        for (const char* k : kws) {
            if (!k) continue;
            if (!first) keywords += ",";
            keywords += k;
            first = false;
        }
    }
    keywords += "]";
    // the private slot carries per-card-family accumulators (Rampage ExtraDamageFromPlays;
    // Claw/Maul/GeneticAlgorithm/TheScythe report their var bump as the accumulator)
    std::string priv = "-";
    switch (c.id) {
        case CardId::RAMPAGE: priv = std::to_string(c.privateAccum); break;
        case CardId::CLAW:
        case CardId::MAUL:
        case CardId::GENETIC_ALGORITHM:
        case CardId::THE_SCYTHE: priv = std::to_string(c.varBump[0]); break;
        default: break;
    }
    // EnergyCost.CostsX / HasStarCostX come from the card's own flags (VOLLEY is X-cost);
    // replay count is 0 for a fresh instance. The retain slot is
    // CardModel.ShouldRetainThisTurn: the Retain keyword, a persistent instance retain, or a
    // single-turn retain (Forged Sovereign Blade stamps Retain=True).
    const char* costsX = d.hasFlag(CF_ENERGY_X) ? "True" : "False";
    const char* starX = d.hasFlag(CF_STAR_X) ? "True" : "False";
    const bool retains = c.retain || c.retainThisTurn || cardHasFlag(c, CF_RETAIN);
    // Affliction slot: "-:0" or "ENTANGLED:N" (TangledPower applies Entangled to attacks).
    const std::string affliction = c.affliction == 1
        ? "ENTANGLED:" + std::to_string(c.afflictionAmount)
        : c.affliction == 2 ? "RINGING:" + std::to_string(c.afflictionAmount)
        : c.affliction == 3 ? "TAINTED:" + std::to_string(c.afflictionAmount)
        : c.affliction == 4 ? "BOUND:" + std::to_string(c.afflictionAmount)
        : c.affliction == 5 ? "HEXED:" + std::to_string(c.afflictionAmount)
        : c.affliction == 6 ? "GALVANIZED:" + std::to_string(c.afflictionAmount) : "-:0";
    // in-combat cost deltas (EnergyCost.AddThisCombat) surface as private|cost-state and
    // drag the stars slot along, matching the game's entry shape
    std::string starsSeg;
    if (c.costBase >= 0 || !c.costMods.empty()) {
        int base = c.costBase >= 0 ? c.costBase : d.cost + d.costUp * c.upgrade;
        std::string state = std::to_string(base) + ":" + std::to_string(c.costMods.size()) + "[";
        for (int m : c.costMods) state += std::to_string(m) + ":2:0:False,";
        state += "]";
        priv += "|cost-state=" + state;
        starsSeg = "/stars=" + std::to_string(d.starCost) + ":0[]";
    }
    return std::string(CardId_NAMES[(int)c.id]) + "+" + std::to_string(c.upgrade)
         + "/" + costsX + ":" + buf + "/" + starX + ":" + std::to_string(d.starCost)
         + "/" + std::to_string(c.extraReplays) + "/" + (c.exhaust ? "True" : "False") + "/" + (c.sly ? "True" : "False")
         + "/" + (retains ? "True" : "False")
         + "/" + (deckFlag ? "True" : "False")
         + "/False/-:0:0/" + affliction + vars + "/private=" + priv + starsSeg
         + "/keywords=" + keywords + "/baselib=-";
}

bool parseStamp(const std::string& stamp, BattleContext& bc, std::string* err) {
    bc = BattleContext{};
    auto fields = split(stamp, ';');
    std::string piles[4]; // H D C X
    std::string ai, ms, potions, powers, rngs, orbsRaw;
    std::vector<std::string> eEntries;

    for (auto& f : fields) {
        auto eq = f.find('=');
        if (eq == std::string::npos) continue;
        std::string key = f.substr(0, eq), val = f.substr(eq + 1);
        if (key == "turn") bc.turn = std::atoi(val.c_str());
        else if (key == "hp") bc.player.curHp = std::atoi(val.c_str());
        else if (key == "max_hp") bc.player.maxHp = std::atoi(val.c_str());
        else if (key == "block") bc.player.block = std::atoi(val.c_str());
        else if (key == "energy") bc.player.energy = std::atoi(val.c_str());
        else if (key == "stars") bc.player.stars = std::atoi(val.c_str());
        else if (key == "gold") bc.player.gold = std::atoi(val.c_str());
        else if (key.size() >= 1 && key[0] == 'E' && key != "energy" && key.substr(0,6) != "energy") {
            eEntries.push_back(val);
        }
        else if (key[0] == 'A' && key.size() > 1 && key[1] == 'I') ai = val;
        else if (key[0] == 'M' && key.size() > 1 && key[1] == 'S') ms = val;
        else if (key == "H") piles[0] = val;
        else if (key == "D") piles[1] = val;
        else if (key == "C") piles[2] = val;
        else if (key == "X") piles[3] = val;
        else if (key == "potions") potions = val;
        else if (key == "P") powers = val;
        else if (key == "R") rngs = val;
        else if (key == "O") orbsRaw = val;   // "capacity:NAME[passive/evoke],..."
        else if (key == "osty") {
            // osty=cid/hp/maxhp (cid 4294967295 = absent)
            auto parts = split(val, '/');
            if (parts.size() >= 3) {
                unsigned long cid = std::strtoul(parts[0].c_str(), nullptr, 10);
                bc.player.osty.combatId = (cid == 4294967295UL) ? 0 : (int)cid;
                bc.player.osty.curHp = std::atoi(parts[1].c_str());
                bc.player.osty.maxHp = std::atoi(parts[2].c_str());
                bc.player.osty.present = (cid != 4294967295UL);
                bc.player.ownerHasBoundPhylactery = bc.player.osty.present;
                // DieForYouPower is granted by OstyCmd.Summon and normally appears in the
                // P= list under the pet's cid; add it only when the stamp omitted it.
                if (bc.player.osty.combatId >= bc.nextCombatId) bc.nextCombatId = bc.player.osty.combatId;
            }
        }
        else if (key == "max_hand_size") bc.maxHandSize = std::atoi(val.c_str());
        else if (key == "relicCounters") {
            // <RELIC>/<current>/<previous>[,<RELIC>/<current>/<previous>...]
            size_t pos = 0;
            while (pos <= val.size()) {
                const size_t comma = val.find(',', pos);
                const std::string item = val.substr(
                    pos, comma == std::string::npos ? std::string::npos : comma - pos);
                const size_t s1 = item.find('/');
                const size_t s2 = s1 == std::string::npos ? std::string::npos
                                                          : item.find('/', s1 + 1);
                if (s1 != std::string::npos) {
                    const std::string nm = item.substr(0, s1);
                    const int cur = std::atoi(item.substr(s1 + 1).c_str());
                    const int prev = s2 == std::string::npos ? 0
                        : std::atoi(item.substr(s2 + 1).c_str());
                    if (nm == "ART_OF_WAR") {
                        bc.relicAoWThisTurn = cur != 0;
                        bc.relicAoWLastTurn = prev != 0;
                    } else if (nm == "HAPPY_FLOWER") {
                        bc.relicHappyTurns = cur;
                    } else if (nm == "BELT_BUCKLE") {
                        bc.relicBeltBuckleApplied = cur != 0;
                    } else if (nm == "RAINBOW_RING") {
                        bc.relicRainbowActivated = cur;
                    } else if (nm == "PENDULUM") {
                        bc.relicPendulumTurns = cur;
                    } else if (nm == "POCKETWATCH") {
                        bc.relicPocketwatchThisTurn = cur;
                        bc.relicPocketwatchLastTurn = prev;
                    } else if (nm == "RUINED_HELMET") {
                        bc.relicRuinedHelmetUsed = cur != 0;
                    } else if (nm == "UNSETTLING_LAMP") {
                        bc.relicLampFinished = cur != 0;
                    } else if (nm == "RED_SKULL") {
                        bc.redSkullActive = cur != 0;
                    }
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
        else if (key == "mirrorRelics") {
            // <RELIC>/<counter>[,<RELIC>/<counter>...]
            size_t pos = 0;
            while (pos <= val.size()) {
                const size_t comma = val.find(',', pos);
                const std::string item = val.substr(
                    pos, comma == std::string::npos ? std::string::npos : comma - pos);
                const size_t slash = item.find('/');
                if (slash != std::string::npos) {
                    const std::string nm = item.substr(0, slash);
                    const int v = std::atoi(item.substr(slash + 1).c_str());
                    if (nm == "BEATING_REMNANT") bc.relicBeatingDamageThisTurn = v;
                    else if (nm == "BURNING_STICKS")
                        bc.relicBurningSticksUsed = item.substr(slash + 1) == "1";
                    else if (nm == "PERMAFROST")
                        bc.relicPermafrostUsed = item.substr(slash + 1) == "1";
                    else if (nm == "KUNAI" || nm == "SHURIKEN" || nm == "ORNAMENTAL_FAN")
                        bc.relicAttacksThisTurn = v;
                    else if (nm == "LETTER_OPENER") bc.relicSkillsThisTurn = v;
                    else if (nm == "NUNCHAKU") bc.relicNunchakuAttacks = v;
                    else if (nm == "PEN_NIB") bc.relicPenNibAttacks = v;
                    else if (nm == "CENTENNIAL_PUZZLE")
                        bc.relicPuzzleUsed = item.substr(slash + 1) == "1";
                    else if (nm == "DEMON_TONGUE")
                        bc.relicDemonTongueUsed = item.substr(slash + 1) == "1";
                    else if (nm == "JOSS_PAPER") {
                        const std::string rest = item.substr(slash + 1);
                        const size_t et = rest.find(":ethereal=");
                        bc.relicJossExhausted = std::atoi(rest.c_str());
                        bc.relicJossEthereal = et == std::string::npos ? 0
                            : std::atoi(rest.substr(et + 10).c_str());
                    }
                    else if (nm == "TUNING_FORK") bc.relicTuningForkSkills = v;
                    else if (nm == "KUSARIGAMA") bc.relicAttacksThisTurn = v;
                    else if (nm == "VAMBRACE") bc.relicVambraceUsed = item.substr(slash + 1) == "1";
                    else if (nm == "RAINBOW_RING") {
                        int a = 0, sk = 0, p = 0, act = 0;
                        const std::string rest = item.substr(slash + 1);
                        std::sscanf(rest.c_str(), "%d:%d:%d:%d", &a, &sk, &p, &act);
                        bc.relicRainbowAttacks = a;
                        bc.relicRainbowSkills = sk;
                        bc.relicRainbowPowers = p;
                        bc.relicRainbowActivated = act;
                    }
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
        // Y/FlameHp/AttackStarts/osty/relicCounters/mirrorRelics: pass through (T1 zeros)
    }

    if (!parsePileEntries(piles[0], bc.hand, err)) return false;
    if (!parsePileEntries(piles[1], bc.drawPile, err)) return false;
    if (!parsePileEntries(piles[2], bc.discardPile, err)) return false;
    if (!parsePileEntries(piles[3], bc.exhaustPile, err)) return false;
    // uniqueIdx must be unique ACROSS piles, not just within one: per-card history checks
    // (Bolas.BeforeHandDraw: e.CardPlay.Card == this) compare an instance that has since
    // moved between piles, so renumber globally over the parsed piles in AllPiles order.
    {
        int uid = 0;
        for (auto* pile : {&bc.hand, &bc.drawPile, &bc.discardPile, &bc.exhaustPile})
            for (auto& c : *pile) c.uniqueIdx = uid++;
    }

    // CardDrawnEntry history is combat-long and includes the opening hand draw of turn 1
    // (SetupPlayerTurn: handDraw = 5). A root stamp at turn 1 is emitted after that draw,
    // so seed the counter to keep Murder-style multipliers in step with the game.
    if (bc.turn == 1) bc.totalCardsDrawn = 5;

    // enemies
    auto aiParts = split(ai, ';');     // AI0=...;AI1=... handled below instead
    (void)aiParts;
    for (const auto& e : eEntries) {
        auto f = split(e, '/');
        if (f.size() < 7) { if (err) *err = "bad E entry: " + e; return false; }
        Monster m;
        m.combatId = std::atoi(f[0].c_str());
        m.name = f[1];
        m.slot = f[2];
        m.curHp = std::atoi(f[3].c_str());
        m.maxHp = std::atoi(f[4].c_str());
        m.block = std::atoi(f[5].c_str());
        m.nextMove = f[6];
        m.defIdx = BattleContext::monsterDefIdx(m.name);
        if (m.defIdx >= 0) {
            const MonsterDef& md = BattleContext::monsterDef(m.defIdx);
            // locate current state so the machine continues where the game is
            for (size_t i = 0; i < md.states.size(); ++i)
                if (md.states[i].moveId == m.nextMove) { m.curState = (int)i; break; }
        }
        bc.enemies.push_back(m);
        if (m.combatId >= bc.nextCombatId) bc.nextCombatId = m.combatId; // summons continue the id sequence
    }

    // AI histories: AI<i>=m1,m2 — verbatim StateLog (initial entry + roll-time logs;
    // the last entry IS the current intent when it was rolled from a loggable state)
    {
        auto fs = split(stamp, ';');
        for (auto& f : fs) {
            if (f.rfind("AI", 0) == 0 && f.size() > 2 && f.find('=') != std::string::npos) {
                int idx = std::atoi(f.substr(2).c_str());
                auto hist = split(f.substr(f.find('=') + 1), ',');
                if (idx < (int)bc.enemies.size()) {
                    for (auto& h : hist)
                        if (!h.empty() && h != "null") bc.enemies[idx].moveHistory.push_back(h);
                }
            }
        }
    }

    // potions
    {
        auto slots = split(potions, ',');
        for (size_t i = 0; i < slots.size() && i < bc.player.potions.size(); ++i)
            bc.player.potions[i] = slots[i] == "-" ? 0 : lookupPotionId(slots[i].c_str());
    }

    // orbs: O=<capacity>:NAME[passive/evoke],NAME[...],
    if (!orbsRaw.empty()) {
        auto colon = orbsRaw.find(':');
        std::string capStr = colon == std::string::npos ? orbsRaw : orbsRaw.substr(0, colon);
        bc.player.orbCapacity = std::atoi(capStr.c_str());
        std::string list = colon == std::string::npos ? std::string() : orbsRaw.substr(colon + 1);
        for (const auto& entry : splitCardEntries(list)) {
            if (entry.empty()) continue;
            auto br = entry.find('[');
            std::string nm = br == std::string::npos ? entry : entry.substr(0, br);
            OrbKind k = orbFromStampName(nm);
            if (k == OrbKind::NONE) continue;
            OrbInstance o;
            o.kind = k;
            // [passive/evoke]: Dark's evoke is the live accumulator; Glass' passive the
            // live decaying value. Other orbs store nothing extra.
            if (br != std::string::npos) {
                auto close = entry.find(']', br);
                std::string nums = entry.substr(br + 1,
                    close == std::string::npos ? std::string::npos : close - br - 1);
                auto sl = nums.find('/');
                if (sl != std::string::npos) {
                    std::string pas = nums.substr(0, sl), evo = nums.substr(sl + 1);
                    if (k == OrbKind::DARK) o.darkEvoke = std::atoi(evo.c_str());
                    if (k == OrbKind::GLASS) o.glassPassive = std::atoi(pas.c_str());
                }
            }
            bc.player.orbs.push_back(o);
        }
    }

    // powers: P= entries "cid:NAME=amount/0[]", comma separated
    if (!powers.empty()) {
        for (const auto& p : splitCardEntries(powers)) {
            if (p.empty()) continue;
            auto cidEnd = p.find(':');
            if (cidEnd == std::string::npos) continue;
            int cid = std::atoi(p.substr(0, cidEnd).c_str());
            std::string rest = p.substr(cidEnd + 1);
            auto eq = rest.find('=');
            if (eq == std::string::npos) continue;
            std::string pname = rest.substr(0, eq);
            std::string amtStr = rest.substr(eq + 1);
            int amount = std::atoi(amtStr.c_str());
            PowerId pid = powerIdFromGameName(pname);
            // dynamic var payload e.g. "[CardsLeft=6,]"
            int misc = -1;
            {
                auto b = amtStr.find('[');
                if (b != std::string::npos) {
                    auto eq = amtStr.find('=', b);
                    auto endc = amtStr.find(',', eq == std::string::npos ? b : eq);
                    if (eq != std::string::npos)
                        misc = std::atoi(amtStr.substr(eq + 1, endc == std::string::npos ? std::string::npos : endc - eq - 1).c_str());
                }
            }
            auto applyToList = [&](std::vector<PowerInst>& list) {
                addPower(list, pid, amount, false);
                // set the payload on the entry just added (instanced powers append, so the
                // LAST matching entry is ours; stacked powers have exactly one)
                for (auto it = list.rbegin(); it != list.rend(); ++it)
                    if (it->id == pid) { it->misc = misc; it->aux = misc; break; }
            };
            if (cid == 0) {
                if (pid != PowerId::NONE) applyToList(bc.player.powers);
            } else if (bc.player.osty.present && cid == bc.player.osty.combatId) {
                // the Necrobinder pet is its own creature: its powers carry its combat id
                if (pid != PowerId::NONE) applyToList(bc.player.osty.powers);
            } else {
                for (auto& m : bc.enemies)
                    if (m.combatId == cid && pid != PowerId::NONE) applyToList(m.powers);
            }
        }
    }

    // rng streams
    if (!rngs.empty()) {
        auto streams = split(rngs, '/');
        if (streams.size() != RNG_STREAM_COUNT) {
            if (err) *err = "expected 9 rng streams, got " + std::to_string(streams.size());
            return false;
        }
        for (int i = 0; i < RNG_STREAM_COUNT; ++i)
            if (!parseRngState(streams[i], bc.rng[i], err)) return false;
    }

    return true;
}

std::string emitStamp(const BattleContext& bc) {
    std::string s;
    s += "turn=" + std::to_string(bc.turn);
    s += ";hp=" + std::to_string(bc.player.curHp);
    s += ";max_hp=" + std::to_string(bc.player.maxHp);
    s += ";block=" + std::to_string(bc.player.block);
    s += ";energy=" + std::to_string(bc.player.energy);
    s += ";stars=" + std::to_string(bc.player.stars);
    s += ";gold=" + std::to_string(bc.player.gold);
    // osty=cid/hp/maxhp (4294967295 = no pet in combat)
    if (bc.player.osty.present) {
        s += ";osty=" + std::to_string(bc.player.osty.combatId) + "/"
           + std::to_string(bc.player.osty.curHp) + "/" + std::to_string(bc.player.osty.maxHp);
    } else {
        s += ";osty=4294967295/0/0";
    }

    int ei = 0;
    for (size_t i = 0; i < bc.enemies.size(); ++i) {
        const Monster& m = bc.enemies[i];
        if (m.removed) continue;
        s += ";E" + std::to_string(ei) + "=" + std::to_string(m.combatId) + "/" + m.name + "/" + m.slot
           + "/" + std::to_string(m.curHp) + "/" + std::to_string(m.maxHp) + "/" + std::to_string(m.block)
           + "/" + (m.nextMove.empty() ? std::string("null") : m.nextMove);
        ++ei;
    }
    ei = 0;
    for (size_t i = 0; i < bc.enemies.size(); ++i) {
        const Monster& m = bc.enemies[i];
        if (m.removed) continue;
        std::string hist;
        for (size_t k = 0; k < m.moveHistory.size(); ++k) {
            if (k) hist += ",";
            hist += m.moveHistory[k];
        }
        s += ";AI" + std::to_string(ei) + "=" + hist; // StateLog verbatim
        ++ei;
    }
    ei = 0;
    for (size_t i = 0; i < bc.enemies.size(); ++i) {
        const Monster& m = bc.enemies[i];
        if (m.removed) continue;
        std::string msName = m.msName;
        if (std::strcmp(m.name.c_str(), "TUNNELER") == 0)
            msName = m.tunnelerStunned ? "1" : "0";
        if (std::strcmp(m.name.c_str(), "TOUGH_EGG") == 0)
            msName = m.eggHatched ? "1" : "0";
        if (std::strcmp(m.name.c_str(), "SLUMBERING_BEETLE") == 0)
            msName = m.isAwake ? "1" : "0";
        if (std::strcmp(m.name.c_str(), "FROG_KNIGHT") == 0)
            msName = m.beetleCharged ? "1" : "0";
        if (std::strcmp(m.name.c_str(), "FABRICATOR") == 0)
            msName = m.lastSpawned.empty() ? "-" : m.lastSpawned;
        if (std::strcmp(m.name.c_str(), "TEST_SUBJECT") == 0)
            msName = std::to_string(m.respawns) + "," + std::to_string(m.extraMultiClawCount);
        if (std::strcmp(m.name.c_str(), "QUEEN") == 0)
            msName = m.amalgamDied ? "1" : "0";
        s += ";MS" + std::to_string(ei) + "=" + msName + "/" + std::to_string(m.msMisc);
        ++ei;
    }

    auto pileStr = [](const std::vector<CardInstance>& pile) {
        std::string out;
        for (auto& c : pile) out += emitCardEntry(c) + ",";
        return out;
    };
    s += ";H=" + pileStr(bc.hand);
    s += ";D=" + pileStr(bc.drawPile);
    s += ";C=" + pileStr(bc.discardPile);
    s += ";X=" + pileStr(bc.exhaustPile);
    {   // Y = turn card history (player): status draws / zero-cost attack starts /
        // card play starts / attack+skill starts (UnattendedTestRunner.ReplayState)
        s += ";Y=" + std::to_string(bc.statusDrawsThisTurn)
           + "/" + std::to_string(bc.zeroCostAttackStarts)
           + "/" + std::to_string(bc.cardPlayStarts)
           + "/" + std::to_string(bc.attackSkillStarts);
        // FlameHp = BrightestFlame MaxHp spend (card not in the supported set: always 0)
        s += ";FlameHp=0";
        s += ";AttackStarts=" + std::to_string(bc.attackStartsThisTurn);
    }
    s += ";O=" + std::to_string(bc.player.orbCapacity) + ":";
    {
        // OrbQueue order: index 0 = next orb. Values are the Focus-adjusted stamped
        // values (game emits orb.PassiveVal/EvokeVal, both ModifyOrbValue results).
        for (const auto& o : bc.player.orbs) {
            s += orbStampName(o.kind);
            s += "[" + std::to_string(bc.orbValue(o, /*evoke=*/false))
               + "/" + std::to_string(bc.orbValue(o, /*evoke=*/true)) + "],";
        }
    }
    {
        std::string p;
        for (size_t i = 0; i < bc.player.potions.size(); ++i) {
            if (i) p += ",";
            p += bc.player.potions[i] == 0 ? "-" : PotionId_NAMES[bc.player.potions[i]];
        }
        s += ";potions=" + p;
    }
    {
        // mirrorRelics: the harness's tracked-relic counter snapshot (sorted by name).
        // Only relics the engine tracks are emitted; state-changing effects are already in
        // the fields above.
        std::string m;
        auto addMirrorText = [&](RelicId id, const std::string& text) {
            if (!bc.hasRelic(id)) return;
            if (!m.empty()) m += ",";
            m += std::string(sts2RelicName(id)) + "/" + text;
        };
        auto addMirror = [&](RelicId id, int value) {
            if (!bc.hasRelic(id)) return;
            if (!m.empty()) m += ",";
            m += std::string(sts2RelicName(id)) + "/" + std::to_string(value);
        };
        addMirror(RelicId::BEATING_REMNANT, bc.relicBeatingDamageThisTurn);
        addMirrorText(RelicId::BURNING_STICKS, bc.relicBurningSticksUsed ? "1" : "0");
        addMirrorText(RelicId::CENTENNIAL_PUZZLE, bc.relicPuzzleUsed ? "1" : "0");
        addMirrorText(RelicId::DEMON_TONGUE, bc.relicDemonTongueUsed ? "1" : "0");
        {
            // JossPaperText: "<exhausted>" or "<exhausted>:ethereal=<n>".
            std::string jp = std::to_string(bc.relicJossExhausted);
            if (bc.relicJossEthereal != 0)
                jp += ":ethereal=" + std::to_string(bc.relicJossEthereal);
            addMirrorText(RelicId::JOSS_PAPER, jp);
        }
        addMirror(RelicId::KUNAI, bc.relicAttacksThisTurn);
        addMirror(RelicId::KUSARIGAMA, bc.relicAttacksThisTurn);
        addMirror(RelicId::LETTER_OPENER, bc.relicSkillsThisTurn);
        addMirror(RelicId::NUNCHAKU, bc.relicNunchakuAttacks);
        addMirror(RelicId::ORNAMENTAL_FAN, bc.relicAttacksThisTurn);
        addMirror(RelicId::PEN_NIB, bc.relicPenNibAttacks);
        {
            // RainbowRingText: "<attacks>:<skills>:<powers>:<activation>".
            addMirrorText(RelicId::RAINBOW_RING,
                          std::to_string(bc.relicRainbowAttacks) + ":"
                          + std::to_string(bc.relicRainbowSkills) + ":"
                          + std::to_string(bc.relicRainbowPowers) + ":"
                          + std::to_string(bc.relicRainbowActivated));
        }
        addMirrorText(RelicId::PERMAFROST, bc.relicPermafrostUsed ? "1" : "0");
        addMirror(RelicId::SHURIKEN, bc.relicAttacksThisTurn);
        addMirror(RelicId::TUNING_FORK, bc.relicTuningForkSkills);
        addMirrorText(RelicId::VAMBRACE, bc.relicVambraceUsed ? "1" : "0");
        // Stateful relic counters (current/previous), sorted by name.
        std::string rc;
        auto addCounters = [&](RelicId id, int cur, int prev) {
            if (!bc.hasRelic(id)) return;
            if (!rc.empty()) rc += ",";
            rc += std::string(sts2RelicName(id)) + "/" + std::to_string(cur)
                + "/" + std::to_string(prev);
        };
        addCounters(RelicId::ART_OF_WAR, bc.relicAoWThisTurn ? 1 : 0,
                    bc.relicAoWLastTurn ? 1 : 0);
        addCounters(RelicId::BELT_BUCKLE, bc.relicBeltBuckleApplied ? 1 : 0, 0);
        addCounters(RelicId::HAPPY_FLOWER, bc.relicHappyTurns, 0);
        addCounters(RelicId::PENDULUM, bc.relicPendulumTurns, 0);
        addCounters(RelicId::POCKETWATCH, bc.relicPocketwatchThisTurn,
                    bc.relicPocketwatchLastTurn);
        addCounters(RelicId::RAINBOW_RING, bc.relicRainbowActivated, 0);
        addCounters(RelicId::RED_SKULL, bc.redSkullActive ? 1 : 0, 0);
        addCounters(RelicId::RUINED_HELMET, bc.relicRuinedHelmetUsed ? 1 : 0, 0);
        addCounters(RelicId::UNSETTLING_LAMP, bc.relicLampFinished ? 1 : 0, 0);
        s += ";relicCounters=" + rc + ";mirrorRelics=" + m;
    }

    // powers: player first (cid 0), then enemies in order
    {
        std::string p;
        auto emitOne = [&](int cid, const PowerInst& pi) {
            p += std::to_string(cid) + ":" + powerName(pi.id) + "=" + std::to_string(pi.amount) + "/0";
            if (pi.id == PowerId::WITHERING_PRESENCE)
                p += "[CardsLeft=" + std::to_string(pi.misc < 0 ? pi.amount : pi.misc) + ",]";
            else if (pi.id == PowerId::PANACHE)
                // Instanced: Amount is the damage, CardsLeft the countdown to the next hit
                p += "[CardsLeft=" + std::to_string(pi.aux < 0 ? 5 : pi.aux) + ",]";
            else if (pi.id == PowerId::THE_BOMB)
                // Instanced: Amount = turns left, Damage = the payload
                p += "[Damage=" + std::to_string(pi.aux < 0 ? 40 : pi.aux) + ",]";
            else if (pi.id == PowerId::AUTOMATION)
                // Instanced: Amount = energy granted; BaseCards is the static canonical
                // dynvar (10) — the per-instance countdown lives in Data.cardsLeft and is
                // NOT the stamped value (stamp shows BaseCards=10 even at 5 cards left)
                p += "[BaseCards=10,]";
            else if (pi.id == PowerId::ROLLING_BOULDER)
                // Instanced: Amount IS the damage; the dynvar shows the per-application step
                p += "[Damage=" + std::to_string(pi.aux < 0 ? 5 : pi.aux) + ",]";
            else if (pi.id == PowerId::DANSE_MACABRE)
                p += "[Energy=2,]"; // canonical Energy dynvar (card upgrade only bumps Amount)
            else if (pi.id == PowerId::COLOSSUS)
                p += "[DamageDecrease=0.5,]"; // canonical dynvar
            else if (pi.id == PowerId::HAILSTORM)
                p += "[FrostOrbs=1,]"; // canonical var (static threshold)
            else if (pi.id == PowerId::ORBIT) {
                // Energy = canonical var; the harness stamps (4 - DisplayAmount) % 4 where
                // DisplayAmount = 4 - energySpent % 4, i.e. the instance's own spent % 4
                int spent = pi.misc < 0 ? 0 : pi.misc;
                p += "[Energy=4,EnergyRemainder=" + std::to_string(spent % 4) + ",]";
            }
            else if (pi.id == PowerId::WEAK)
                p += "[DamageDecrease=0.75,]";
            else if (pi.id == PowerId::VULNERABLE)
                p += "[DamageIncrease=1.5,]";
            else if (pi.id == PowerId::SHRINK)
                p += "[DamageDecrease=30,]"; // canonical dynvar (ShrinkPower.cs)
            else if (pi.id == PowerId::FLUTTER)
                p += "[DamageDecrease=50,]"; // canonical dynvar (FlutterPower.cs)
            else if (pi.id == PowerId::SOAR)
                p += "[DamageDecrease=50,]"; // canonical dynvar (SoarPower.cs)
            else if (pi.id == PowerId::SURROUNDED)
                p += std::string("[Facing=") + (pi.misc == 1 ? "Left" : "Right") + ",]";
            else if (pi.id == PowerId::CRAB_RAGE)
                p += "[Block=99,StrengthPower=6,]"; // canonical dynvars (CrabRagePower.cs)
            else if (pi.id == PowerId::SLOW) {
                // Live dynvars: SlowAmount = cards played this side turn (reset at enemy
                // side start), DisplayAmount = SlowAmount * 10.
                const int slow = pi.misc < 0 ? 0 : pi.misc;
                p += "[DisplayAmount=" + std::to_string(slow * 10) + ",SlowAmount=" +
                     std::to_string(slow) + ",]";
            }
            else if (pi.id == PowerId::TANGLED)
                p += "[Energy=1,]"; // canonical EnergyVar(1) (TangledPower.cs)
            else if (pi.id == PowerId::INFERNO || pi.id == PowerId::CRIMSON_MANTLE)
                p += "[SelfDamage=" + std::to_string(pi.misc < 0 ? 0 : pi.misc) + ",]";
            else if (pi.id == PowerId::PLATING)
                p += "[Decrement=1,]"; // single player: canonical dynvar stays 1
            else if (pi.id == PowerId::TANK)
                p += "[DamageIncrease=1.5,DamageDecrease=0.5,]";
            else
                p += "[]";
            p += ",";
        };
        for (auto& pi : bc.player.powers) emitOne(0, pi);
        // the Necrobinder pet is a real creature: its powers carry its own combat id
        if (bc.player.osty.present)
            for (auto& pi : bc.player.osty.powers) emitOne(bc.player.osty.combatId, pi);
        for (auto& m : bc.enemies)
            if (!m.removed)
                for (auto& pi : m.powers) emitOne(m.combatId, pi);
        s += ";P=" + p;
    }

    {
        std::string r;
        for (int i = 0; i < RNG_STREAM_COUNT; ++i) {
            if (i) r += "/";
            r += emitRngState(bc.rng[i]);
        }
        s += ";R=" + r;
    }
    s += ";max_hand_size=" + std::to_string(bc.maxHandSize);
    return s;
}

} // namespace sts2
