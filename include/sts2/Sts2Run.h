// STS2 run layer (T4 scaffold) — map → rooms → combats → rewards → act transition.
//
// Authority: data_private/sts2_decompiled (RunState/RunManager/ActMap/EncounterModel).
// Current scope is a deterministic full-run loop over the monster families the engine
// already implements. Encounter composition comes from the frozen parity corpus root
// stamps (the game's own output), and encounter selection is a scaffold table — the
// real UpFront GrabBag selection is filled in with T2-E content.

#ifndef STS2_RUN_H
#define STS2_RUN_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sts2/Sts2Battle.h"
#include "sts2/Sts2Map.h"
#include "sts2/Sts2Random.h"
#include "sts2/Sts2Potion.h"
#include "sts2/Sts2Relic.h"

namespace sts2 {

struct RunCard {
    CardId id = CardId::INVALID;
    int upgrade = 0;
    // EnchantmentModel on the deck card (EnchantmentId value; 0 = none).
    int enchant = 0;
    int enchantAmount = 0;
    bool enchantDisabled = false; // Sown's one-shot status, persisted across combats
    // TinkerTime's MadScience configuration (type 0/1/2, rider enum 0-9).
    int tinkerType = 0;
    int tinkerRider = 0;
};

struct RunPlayer {
    std::string character = "IRONCLAD";
    int hp = 80;
    int maxHp = 80;
    int gold = 99;
    std::vector<RunCard> deck;
    std::array<int, 3> potions{}; // PotionId values, 0 = empty slot
};

struct EncounterDef {
    const char* id;
    bool elite;
    bool boss;
    int act = -1;   // 0-based act the encounter belongs to (-1: any)
    // Dynamic compositions (the game's GenerateMonsters rolls per encounter):
    // "" = use `monsters`; "slimes_weak"/"slimes_normal" = rolled from the encounter RNG.
    const char* generator = "";
    // (monster name, slot). A slot prefixed with '@' selects a starter move index:
    // "@1" -> starterMoveIdx = 1 (Nibbit front/back INIT conditional).
    std::vector<std::pair<const char*, const char*>> monsters;
    // Event combats (BattlewornDummy) give no room rewards of their own. Kept last so the
    // existing positional initializers stay valid.
    bool noRewards = false;
    // Fixed room gold reward (FakeMerchant's encounter pays exactly 300); -1 = rolled.
    int goldReward = -1;
};

struct RoomLog {
    int floor = 0;
    MapPointType type = MapPointType::Unassigned;
    const char* encounter = "";
    bool won = false;
    int hpAfter = 0;
    int turns = 0;
    int goldReward = 0;              // GoldReward after this room (0 for non-combat rooms)
    bool potionReward = false;       // PotionReward roll outcome
    std::vector<std::pair<int,int>> cardChoices; // offered (CardId, upgrade)
};

// One merchant shelf slot. Cards use cardId/upgrade; the card-removal service sets
// `removal` (and cardId = INVALID). Prices follow MerchantCardEntry.CalcCost.
struct ShopEntry {
    int cardId = 0;      // CardId (0 = none)
    int upgrade = 0;
    int price = 0;
    bool onSale = false;
    bool sold = false;
    bool removal = false;
    bool relic = false;  // relic shelf slot (relicId)
    int relicId = 0;
    bool potion = false; // potion shelf slot (potionId)
    int potionId = 0;
};

struct RunState {
    std::string stringSeed;
    std::uint64_t seed = 0;
    RunPlayer player;
    int actIndex = 0;          // 0..2
    std::string actName = "OVERGROWTH";
    ActMap map;
    int current = -1;          // current map point index (-1 before the first step)
    int totalFloor = 0;
    int actFloor = 0;               // floors inside the current act (obs v8 base block)
    std::vector<int> visitedPoints; // map point indices visited this act (obs v8 map block)
    bool over = false;
    bool won = false;
    std::array<Sts2Rng, RNG_STREAM_COUNT> rng;
    Sts2Rng upFront;          // RunRngSet.UpFront: room contents
    Sts2Rng rewards;          // PlayerRngSet.Rewards: gold, potions, card rewards
    Sts2Rng shops;            // PlayerRngSet.Shops: merchant inventory and prices
    Sts2Rng unknownMapPoint;  // RunRngSet.UnknownMapPoint: ? room type roll
    float potionOdds = 0.4f;  // PotionRewardOdds pity state (base 0.4)
    float cardRarityOffset = -0.05f; // CardRarityOdds accumulating rare offset
    std::string bossOverride; // test hook: force the boss-room encounter id
    std::string eventOverride; // test hook: force the ? room event id (gates still apply)
    int ascension = 0;        // STS2 ascension 0-10 (A8/A9 swap the monster registry)
    int potionSlots = 3;      // AscensionManager TightBelt/A4: 3 -> 2
    std::string secondBossExclude; // A10: the first boss id, excluded from the second pick
    std::vector<RoomLog> rooms;
    int lastPerfectFitMoves = 0; // diagnostic: PerfectFit top-of-pile moves in the last fight
    int lastCardsGenerated = 0;  // diagnostic: cardsGeneratedByOwner in the last fight
    std::string lastUnsupportedCard; // diagnostic: card that aborted the last fight ("" ok)
    // Sticky across every fight in the run: the first unimplemented card that aborted a
    // play. lastUnsupportedCard is per-fight and the next clean battle overwrites it,
    // which hid DISCOVERY aborts from the training closure counters.
    std::string runUnsupportedCard;
    // Relic counters that the game persists across combats (Pen Nib / Nunchaku).
    int penNibAttacks = 0;
    int nunchakuAttacks = 0;
    int happyFlowerTurns = 0; // Happy Flower TurnsSeen persists across combats
    int tuningForkSkills = 0; // Tuning Fork SkillsPlayed persists across combats
    int pendulumTurns = 0;    // Pendulum TurnsSeen persists across combats
    int jossPaperExhausted = 0; // Joss Paper CardsExhausted persists across combats
    bool venerableTeaEnergy = false; // Venerable Tea Set: +1 energy next combat
    int lastingCandyRewards = 0; // Lasting Candy CombatRewardsSeen (extra card on odd)
    int lastSharpApplications = 0;  // diagnostic: Sharp damage bonuses in the last fight
    int lastNimbleApplications = 0; // diagnostic: Nimble block bonuses in the last fight
    int lastSlitherDraws = 0;       // diagnostic: Slither draws in the last fight
    int lastToricRegrants = 0;      // diagnostic: ToricToughness re-grants in the last fight
    int lastSpiralReplays = 0;      // diagnostic: Spiral extra plays in the last fight
    int lastVigorousApplications = 0; // diagnostic: Vigorous one-shot bonuses
    int lastCorruptedTriggers = 0;    // diagnostic: Corrupted 2-HP payments
    int lastSoulsPowerKept = 0;       // diagnostic: SoulsPower cards kept from exhaust
    bool lastFightWon = false;        // diagnostic: outcome of the last fight
    bool lastFightTimedOut = false;   // diagnostic: BattlewornDummy time limit
    int lastFightTurns = 0;           // diagnostic: turns the last fight lasted
    int lastBattleStartEnergy = 0;    // diagnostic: first-turn energy of the last fight
    int lastFightEnemyHpLeft = 0;     // diagnostic: surviving enemy HP after the last fight

    // ---- interactive decision surface (T4) ----
    // A room that needs a policy decision stops with the matching flag set; the caller
    // resolves it and the flag clears. withDefaultDecisions() applies the scaffold policy.
    bool awaitingCardReward = false;
    std::vector<std::pair<int,int>> cardOffer; // offered (CardId, upgrade) after a win
    // Optional rewards from the same reward screen. The game shows potion, card and relic
    // in that order; each may be taken or skipped. Resolve potion before the card and the
    // relic after it (the caller drives the order).
    bool awaitingPotionReward = false;
    int potionRewardId = 0;
    bool awaitingRelicReward = false;
    int relicRewardId = 0;
    bool awaitingRest = false;
    int restHeal = 0;                // HealRestSiteOption.GetBaseHealAmount = 30% max HP
    std::vector<int> restUpgradable; // deck indices with a remaining upgrade level
    bool awaitingShop = false;
    // Toolbox: the battle starts after the player picks one of three colorless cards.
    bool awaitingToolbox = false;
    std::vector<int> toolboxOptions; // CardId values of the three offered cards
    int toolboxPoint = -1;           // map point waiting to be entered
    int toolboxResumePoint = -1;     // set while re-entering that point after the pick
    long long toolboxSims = 0;
    int toolboxEncIdx = -1;          // cached encounter-table index
    std::string toolboxEncId;        // cached forced encounter id ("" = table index)
    int toolboxCard = 0;             // chosen CardId for the next battle
    std::vector<ShopEntry> shop;     // MerchantInventory: 5 character cards + 2 colorless + removal
    int cardRemovalsUsed = 0;        // ExtraFields.CardShopRemovalsUsed (removal price grows)
    int unknownPointsVisited = 0;    // tutorial rule for the first ? rooms
    // Owned relics (RelicId values, acquisition order) and the RelicGrabBag deques.
    std::vector<int> relics;
    std::array<std::vector<int>, RELIC_RARITY_COUNT> relicBag;
    // UnknownMapPointOdds current values (start at the base odds).
    float oddsMonster = 0.1f;
    float oddsTreasure = 0.02f;
    float oddsShop = 0.03f;
    // Event room (UnknownMapPointOdds rolls Event; DenseVegetation is implemented).
    bool awaitingEvent = false;
    std::string eventId;
    int eventPage = 0;                       // 0 = INITIAL, 1 = REST follow-up
    std::vector<std::string> eventOptions;   // option ids in display order
    // CrystalSphere minigame: an 11x11 fog grid whose cells hold items. `eventSphereCells`
    // stores the item index + 1 at every covered cell (0 = empty); `eventSpherePos`/`W`/`H`
    // describe each item; `eventSphereHidden` is the fog mask. Coordinates are (x, y) as
    // in the source's `cells[x, y]`, with a cell index of `y * 11 + x`.
    std::vector<int> eventSphereCells;      // 121 entries, item index + 1
    std::vector<int> eventSphereHidden;     // 121 entries, 1 = fog
    std::vector<int> eventSpherePos;        // item top-left cell index
    std::vector<int> eventSphereW;          // item width
    std::vector<int> eventSphereH;          // item height
    std::vector<int> eventSphereType;       // item kind (1 relic .. 9 big gold)
    std::vector<int> eventSphereRevealed;   // item indices revealed, in reveal order
    int eventSphereDivinations = 0;
    int eventSphereTool = 2;                // 1 = small, 2 = big (default)
    int eventGold = 0;
    int eventGold2 = 0;    // TeaMaster: EmberTea cost (second gold value)
    int eventRelicId = 0;  // WelcomeToWongos: the featured (rare) relic pulled up front
    int wongoPoints = 0;   // WelcomeToWongos loyalty counter (2000 -> badge relic)
    int eventHeal = 0;
    int eventHpLoss = 0;
    int eventDamage = 0;   // AbyssalBaths current damage (grows by 1 per immerse)
    int eventMaxHp = 0;    // AbyssalBaths max-HP gain per immerse
    // Event card-grid choice (RoomFullOfCheese GORGE): pick `eventCardsLeft` cards from
    // `eventCardPool` into the deck.
    bool awaitingEventCards = false;
    // ADD: pool entries are candidate cards taken into the deck; TRANSFORM: entries are
    // the player's own deck cards, the pick is replaced by a random character-pool card.
    bool eventCardsTransform = false;
    // REMOVE: entries are the player's own deck cards; the pick deletes that deck card.
    bool eventCardsRemove = false;
    // ENCHANT: entries are deck cards; the pick applies eventEnchantId/Amount.
    bool eventCardsEnchant = false;
    int eventEnchantId = 0;
    int eventEnchantAmount = 0;
    // RelicTrader: parallel lists of the offered (owned, new) relics.
    std::vector<int> eventOwnedRelics;
    std::vector<int> eventNewRelics;
    // TheFutureOfPotions: card type (CardType value) per potion option, rolled up front.
    std::vector<int> eventPotionTypes;
    // RanwidTheElder: the potion slot and owned relic chosen up front.
    int eventPotionSlot = -1;
    int eventPickRelicId = 0;
    // EndlessConveyor: dish rotation state (ids as strings for observability).
    int eventConveyorGrabs = 0;
    std::string eventConveyorDish;
    std::string eventConveyorLastDish;
    bool eventConveyorResume = false; // resume the conveyor after a JellyLiver grid
    // TinkerTime: chosen card type and the two offered rider ids.
    int eventTinkerType = 0;
    std::vector<int> eventTinkerRiders;
    // FakeMerchant: the six stocked fake relics (-1 once sold).
    std::vector<int> eventShopRelics;
    // Ancient events (Neow, Darv): relic ids behind the offered options.
    std::vector<int> eventRelicOptions;
    // Darv: the Ancient card locked in for the DustyTome option at generation time.
    int eventDustyTomeCard = 0;
    // SlipperyBridge: the card currently at risk, the holds taken, and cards already
    // skipped by earlier holds.
    int eventBridgeCardIdx = -1;
    int eventBridgeHoldOns = 0;
    std::vector<int> eventBridgeSkipped;
    // Trial: which of the three cases was rolled, plus a queued second card reward
    // (Nondescript guilty offers two three-card rewards in sequence).
    int eventTrialCase = 0;
    std::vector<std::pair<int,int>> cardOffer2;
    bool pendingSecondCardReward = false;
    // Extra card rewards beyond the first two (CrystalSphere can reveal three).
    std::vector<std::vector<std::pair<int,int>>> extraCardOffers;
    // UPGRADE: entries are upgradable deck cards; the pick upgrades one.
    bool eventCardsUpgrade = false;
    std::vector<std::pair<int,int>> eventCardPool; // (CardId, upgrade)
    std::vector<int> eventCardDeckIdx;             // TRANSFORM: deck index per pool slot
    int eventCardsLeft = 0;
    int eventPendingHpLoss = 0;                    // applied after a transform pick
    // TRANSFORM_TO: the pick is replaced by the specific eventTransformToId card.
    bool eventCardsTransformTo = false;
    int eventTransformToId = 0;
    // Astrolabe: the transformed replacement enters the deck already upgraded.
    bool eventTransformUpgrade = false;
    // Claws: the transform-to replacement keeps the original card's upgrade and
    // enchantment when the replacement can carry them.
    bool eventTransformKeep = false;
    int eventPendingGoldCost = 0;                  // applied when the card grid closes
    // EventModel.Rng: seed + hash(eventId), one per event, advanced across event pages
    // (TabletOfTruth picks one random upgradable card per decipher).
    Sts2Rng eventRng;

    // RNG streams are indexed by RngStream; the run owns the nine gameplay streams.
    Sts2Rng& stream(RngStream s) { return rng[static_cast<int>(s)]; }
    const Sts2Rng& stream(RngStream s) const { return rng[static_cast<int>(s)]; }
};

// Runs use the same seed hashing as RunRngSet: Seed = xxhash64(stringSeed).
RunState startRun(const std::string& stringSeed, int hpOverride = 0, int ascension = 0);

// Reachable points from the current position (row-1 entries before the first step).
std::vector<int> availablePoints(const RunState& run);

// Enter one reachable point. The caller supplies how many search simulations each
// combat decision gets. Returns false when the run ended during this room. Combat wins and
// rest sites stop at a pending decision (see RunState.awaiting*); resolve them before the
// next enterPoint.
bool enterPoint(RunState& run, int pointIdx, long long searchSims);

//---- decision resolution ----
// Pick offered card `idx` (-1: skip the reward) from RunState.cardOffer.
bool chooseCardReward(RunState& run, int idx);
// Take or skip the pending potion / relic reward (both are optional in the game).
bool choosePotionReward(RunState& run, bool take);
bool chooseRelicReward(RunState& run, bool take);
// "HEAL" (RestSiteOption HEAL) or "SMITH" with a deck index (upgrade that card).
bool chooseRest(RunState& run, const char* option, int deckIdx);
// Buy a shop slot (idx) — cards go to the deck; the removal entry removes deck[removeIdx].
bool buyShop(RunState& run, int idx, int removeIdx = -1);
// Leave the shop (no more purchases).
void leaveShop(RunState& run);
// Pick an event option by index (the option ids are in RunState.eventOptions).
bool chooseEventOption(RunState& run, int idx, long long searchSims);
// Toolbox: pick one of the three offered colorless cards and resume the room.
bool chooseToolbox(RunState& run, int idx);
// Pick one card from the event's card grid into the deck.
bool chooseEventCard(RunState& run, int idx);
// Start the Neow (Ancient) opening event on a fresh run. The run-start path is opt-in so
// existing seeds keep their current opening; the event itself is fully playable.
bool startNeowOpening(RunState& run);
// Darv (Ancient, act 2+ in the game): opt-in opening/act-transition trigger.
bool startDarvOpening(RunState& run);
// Orobas (Ancient): opt-in opening trigger.
bool startOrobasOpening(RunState& run);
// Vakuu (Ancient): opt-in opening trigger.
bool startVakuuOpening(RunState& run);
// Pael (Ancient): opt-in opening trigger.
bool startPaelOpening(RunState& run);
// Nonupeipe (Ancient, act 3 pool): opt-in opening trigger.
bool startNonupeipeOpening(RunState& run);
// Tanx (Ancient, act 3 pool): opt-in opening trigger.
bool startTanxOpening(RunState& run);
// Tezcatara (Ancient, act 2 pool): opt-in opening trigger.
bool startTezcataraOpening(RunState& run);
// CrystalSphere: reveal grid cell (x, y) with the big (8 neighbours + self) or the small
// (self only) tool. When the last divination is spent the revealed items are granted.
bool chooseSphereCell(RunState& run, int x, int y, int tool);
// Scaffold policy for tools that do not model decisions: take the first card, heal when
// hurt otherwise smith the first upgradable card, repeat until no decision is pending.
void withDefaultDecisions(RunState& run);

// Diagnostic: build the encounter's opening battle state (owned relics included) and
// return its ContinuationStamp without running the fight.
std::string previewBattleStamp(RunState& run, const std::string& encounterId);

// One combat reward set (RewardsSet.GenerateRewardsFor): gold, potion roll, three cards.
struct CombatReward {
    int gold = 0;
    bool potion = false;   // the PotionReward odds roll
    int potionId = 0;      // PotionReward.Populate identity (0 = none)
    int relicId = 0;       // elite RelicReward.PullNextRelicFromFront (0 = none)
    std::vector<std::pair<int,int>> cards; // offered (CardId, upgrade) in order
};

// Generates one reward set against the run's Rewards stream (also used by the run loop).
CombatReward generateCombatReward(RunState& run, bool elite, bool boss,
                                  int goldReward = -1);

// The scaffold encounter table (implemented monster families only).
const std::vector<EncounterDef>& scaffoldEncounters();
const EncounterDef* findEncounter(const std::string& id);

} // namespace sts2

#endif // STS2_RUN_H
