/*
 * Chaos Poker bot — Enhanced C++ implementation (v2)
 *
 * Major improvements over v1:
 *   1. Opponent modeling:  per-seat VPIP, fold rate, aggression, showdown profiles
 *   2. Bluffing:           c-bets, double-barrels, semi-bluffs, position bluffs
 *   3. Draw detection:     flush draws, open-ended straight draws, gutshots, combos
 *   4. More MC sims:       200–600 per street (was 30–50)
 *   5. SHOWDOWN/WINNER:    builds opponent ranges and win profiles
 *   6. Vote disruption:    heavy NO votes when opponents show strength
 *   7. Implied odds:       wider draw calls accounting for future value
 *   8. Opponent-adjusted equity: tighten vs nits, loosen vs maniacs
 *   9. Adaptive bet sizing with randomisation for unpredictability
 *
 * C++ guarantees sub-millisecond startup; even 600 MC sims finish in <5 ms.
 */

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <numeric>
#include <random>
#include <set>
#include <map>
#include <cstring>
#include <cmath>
#include <chrono>

// ---------------------------------------------------------------------------
// Card representation
// ---------------------------------------------------------------------------

struct Card {
    int rank;  // 2–14 (14 = Ace)
    int suit;  // 0–3  (s=0, h=1, d=2, c=3)
    bool operator==(const Card& o) const { return rank == o.rank && suit == o.suit; }
    bool operator<(const Card& o) const {
        return rank < o.rank || (rank == o.rank && suit < o.suit);
    }
};

static Card parse_card(const std::string& s) {
    int rank;
    char r = s[0];
    if      (r == 'A') rank = 14;
    else if (r == 'K') rank = 13;
    else if (r == 'Q') rank = 12;
    else if (r == 'J') rank = 11;
    else if (r == 'T') rank = 10;
    else               rank = r - '0';

    int suit;
    char st = s[1];
    if      (st == 's') suit = 0;
    else if (st == 'h') suit = 1;
    else if (st == 'd') suit = 2;
    else                suit = 3;

    return {rank, suit};
}

// Full 52-card deck (built once)
static Card FULL_DECK[52];
static void init_deck() {
    int i = 0;
    for (int r = 2; r <= 14; r++)
        for (int s = 0; s < 4; s++)
            FULL_DECK[i++] = {r, s};
}

// ---------------------------------------------------------------------------
// Hand evaluator — best 5-card hand from 5–7 cards
//
// Returns Score = array<int,6> where [0] = hand type:
//   8=straight flush, 7=quads, 6=full house, 5=flush,
//   4=straight, 3=trips, 2=two pair, 1=pair, 0=high card
// Elements [1..5] are kickers (big-endian).
// ---------------------------------------------------------------------------

using Score = std::array<int, 6>;

static int best_straight(const std::set<int>& rank_set, bool has_ace) {
    for (int top = 14; top >= 6; top--) {
        bool ok = true;
        for (int i = 0; i < 5; i++)
            if (!rank_set.count(top - i)) { ok = false; break; }
        if (ok) return top;
    }
    if (has_ace && rank_set.count(2) && rank_set.count(3) &&
                   rank_set.count(4) && rank_set.count(5))
        return 5;
    return 0;
}

static Score evaluate_cards(const Card* cards, int n) {
    int rank_cnt[15] = {};
    int suit_cnt[4]  = {};
    for (int i = 0; i < n; i++) {
        rank_cnt[cards[i].rank]++;
        suit_cnt[cards[i].suit]++;
    }

    bool has_ace = (rank_cnt[14] > 0);

    std::vector<int> rank_desc;
    rank_desc.reserve(13);
    for (int r = 14; r >= 2; r--)
        if (rank_cnt[r]) rank_desc.push_back(r);

    std::set<int> rank_set(rank_desc.begin(), rank_desc.end());
    int str_high = best_straight(rank_set, has_ace);

    int flush_suit = -1;
    for (int s = 0; s < 4; s++)
        if (suit_cnt[s] >= 5) { flush_suit = s; break; }

    if (flush_suit >= 0) {
        std::vector<int> fr;
        fr.reserve(7);
        for (int r = 14; r >= 2; r--)
            for (int i = 0; i < n; i++)
                if (cards[i].rank == r && cards[i].suit == flush_suit)
                    { fr.push_back(r); break; }

        std::set<int> fset(fr.begin(), fr.end());
        bool fa = fset.count(14) > 0;
        int sf = best_straight(fset, fa);
        if (sf) return {8, sf, 0, 0, 0, 0};

        while ((int)fr.size() > 5) fr.pop_back();
        while ((int)fr.size() < 5) fr.push_back(0);
        return {5, fr[0], fr[1], fr[2], fr[3], fr[4]};
    }

    struct Group { int freq, rank; };
    std::vector<Group> groups;
    groups.reserve(13);
    for (int r = 14; r >= 2; r--)
        if (rank_cnt[r]) groups.push_back({rank_cnt[r], r});
    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        return a.freq > b.freq || (a.freq == b.freq && a.rank > b.rank);
    });

    int g0 = groups[0].freq, r0 = groups[0].rank;
    int g1 = groups.size() > 1 ? groups[1].freq : 0;
    int r1 = groups.size() > 1 ? groups[1].rank  : 0;

    if (g0 == 4) {
        int kicker = (groups.size() > 1) ? r1 : 0;
        return {7, r0, kicker, 0, 0, 0};
    }
    if (g0 == 3 && g1 >= 2)
        return {6, r0, r1, 0, 0, 0};
    if (str_high)
        return {4, str_high, 0, 0, 0, 0};
    if (g0 == 3) {
        std::vector<int> k;
        for (auto& g : groups) if (g.rank != r0) k.push_back(g.rank);
        std::sort(k.rbegin(), k.rend());
        while ((int)k.size() < 2) k.push_back(0);
        return {3, r0, k[0], k[1], 0, 0};
    }
    if (g0 == 2 && g1 == 2) {
        int p1 = std::max(r0, r1), p2 = std::min(r0, r1);
        int kicker = 0;
        for (auto& g : groups) if (g.rank != r0 && g.rank != r1) { kicker = g.rank; break; }
        return {2, p1, p2, kicker, 0, 0};
    }
    if (g0 == 2) {
        std::vector<int> k;
        for (auto& g : groups) if (g.rank != r0) k.push_back(g.rank);
        std::sort(k.rbegin(), k.rend());
        while ((int)k.size() < 3) k.push_back(0);
        return {1, r0, k[0], k[1], k[2], 0};
    }
    std::vector<int> all;
    for (auto& g : groups) all.push_back(g.rank);
    std::sort(all.rbegin(), all.rend());
    while ((int)all.size() < 5) all.push_back(0);
    return {0, all[0], all[1], all[2], all[3], all[4]};
}

// ---------------------------------------------------------------------------
// Draw detection
// ---------------------------------------------------------------------------

struct DrawInfo {
    bool has_flush_draw = false;       // exactly 4 to a flush
    bool has_oesd       = false;       // open-ended straight draw (8 outs)
    bool has_gutshot    = false;       // inside straight draw (4 outs)
    int  flush_suit     = -1;          // suit of the flush draw
    int  flush_outs     = 0;
    int  straight_outs  = 0;
    int  total_outs     = 0;

    bool has_any_draw() const { return has_flush_draw || has_oesd || has_gutshot; }
    bool has_strong_draw() const { return has_flush_draw || has_oesd; }
    bool has_combo_draw() const { return has_flush_draw && (has_oesd || has_gutshot); }
};

static DrawInfo detect_draws(const std::vector<Card>& hole,
                             const std::vector<Card>& community) {
    DrawInfo di;
    if (community.empty()) return di;

    std::vector<Card> all;
    all.insert(all.end(), hole.begin(), hole.end());
    all.insert(all.end(), community.begin(), community.end());

    // --- Flush draw ---
    int suit_cnt[4] = {};
    for (auto& c : all) suit_cnt[c.suit]++;
    for (int s = 0; s < 4; s++) {
        if (suit_cnt[s] == 4) {
            // Check that at least one hole card contributes
            bool hole_contributes = false;
            for (auto& h : hole) if (h.suit == s) { hole_contributes = true; break; }
            if (hole_contributes) {
                di.has_flush_draw = true;
                di.flush_suit     = s;
                di.flush_outs     = 9;
            }
        }
    }

    // --- Straight draw ---
    bool has_rank[16] = {};
    for (auto& c : all) has_rank[c.rank] = true;
    if (has_rank[14]) has_rank[1] = true;  // ace plays low

    int best_cnt = 0;
    int best_top = 0;
    int best_missing_pos = -1;

    for (int top = 14; top >= 5; top--) {
        int cnt = 0;
        int missing_pos = -1;
        for (int i = 0; i < 5; i++) {
            int r = top - i;
            if (r <= 0) break;
            if (has_rank[r]) cnt++;
            else missing_pos = i;
        }
        if (cnt == 4 && cnt > best_cnt) {
            best_cnt = cnt;
            best_top = top;
            best_missing_pos = missing_pos;
        }
        if (cnt == 5) break;  // already have a straight, no draw needed
    }

    if (best_cnt == 4) {
        // Check at least one hole card contributes to the sequence
        bool hole_helps = false;
        for (auto& h : hole) {
            int r = h.rank;
            if (r >= best_top - 4 && r <= best_top) { hole_helps = true; break; }
            if (best_top == 5 && r == 14) { hole_helps = true; break; } // wheel
        }
        if (hole_helps) {
            if (best_missing_pos == 0 || best_missing_pos == 4) {
                di.has_oesd = true;
                di.straight_outs = 8;
            } else {
                di.has_gutshot = true;
                di.straight_outs = 4;
            }
        }
    }

    // Total outs (discount overlap between flush and straight outs)
    di.total_outs = di.flush_outs + di.straight_outs;
    if (di.has_flush_draw && (di.has_oesd || di.has_gutshot))
        di.total_outs -= 2;  // ~2 cards overlap

    return di;
}

// ---------------------------------------------------------------------------
// Board texture (wetness = 0.0 dry … 1.0 very wet)
// ---------------------------------------------------------------------------

static double board_wetness(const std::vector<Card>& community) {
    if (community.size() < 3) return 0.0;

    int suit_cnt[4] = {};
    for (auto& c : community) suit_cnt[c.suit]++;
    double wet = 0.0;

    int max_suited = *std::max_element(suit_cnt, suit_cnt + 4);
    if (max_suited >= 3)      wet += 0.45;
    else if (max_suited >= 2) wet += 0.18;

    std::vector<int> ranks;
    for (auto& c : community) ranks.push_back(c.rank);
    std::sort(ranks.begin(), ranks.end());

    for (int i = 1; i < (int)ranks.size(); i++) {
        int gap = ranks[i] - ranks[i - 1];
        if (gap == 1) wet += 0.18;
        else if (gap == 2) wet += 0.09;
    }

    // Middle cards (8–J) increase connectivity
    for (auto r : ranks)
        if (r >= 8 && r <= 11) wet += 0.04;

    return std::min(1.0, wet);
}

// ---------------------------------------------------------------------------
// Opponent modelling
// ---------------------------------------------------------------------------

struct OpponentStats {
    int hands_seen     = 0;
    int vpip_count     = 0;   // voluntarily put $ in pot (preflop)
    int pfr_count      = 0;   // preflop raise
    int fold_count     = 0;
    int raise_count    = 0;
    int call_count     = 0;
    int check_count    = 0;
    int showdown_count = 0;
    int wins           = 0;
    std::vector<int> showdown_hand_types;  // Score[0] from revealed hands

    int total_actions() const {
        return fold_count + raise_count + call_count + check_count;
    }
    bool has_sufficient_data() const {
        return total_actions() >= 20;
    }
    double fold_rate() const {
        return has_sufficient_data() ? (double)fold_count / total_actions() : 0.15;
    }
    double vpip_rate() const {
        return hands_seen > 5 ? (double)vpip_count / hands_seen : 0.50;
    }
    double pfr_rate() const {
        return hands_seen > 5 ? (double)pfr_count / hands_seen : 0.20;
    }
    double aggression() const {
        int passive = call_count + check_count;
        return passive > 5 ? (double)raise_count / passive : 1.0;
    }
    double avg_showdown_strength() const {
        if (showdown_hand_types.empty()) return 2.0;
        double sum = 0;
        for (auto t : showdown_hand_types) sum += t;
        return sum / showdown_hand_types.size();
    }

    // Estimate how often this opponent bluffs, based on:
    //   - aggression factor (do they bet/raise a lot?)
    //   - showdown hand strength (do they show up with weak hands?)
    // High aggression + weak showdowns = frequent bluffer
    // High aggression + strong showdowns = tight-aggressive (rarely bluffs)
    double estimate_bluff_frequency() const {
        if (!has_sufficient_data() || showdown_count < 3) return 0.25;  // default

        double af = aggression();
        double sd = avg_showdown_strength();

        // High aggression + junk showdowns → confirmed bluffer
        if (af > 1.5 && sd < 1.5)
            return std::min(0.55, 0.25 + (af - 1.0) * 0.15);
        if (af > 1.0 && sd < 2.0)
            return std::min(0.45, 0.20 + (af - 0.8) * 0.10);
        // High aggression + strong showdowns → value bettor, rarely bluffs
        if (af > 1.0 && sd > 3.5)
            return std::max(0.08, 0.20 - (sd - 3.0) * 0.04);
        // Low aggression → passive, rarely bluffs
        if (af < 0.5) return 0.10;

        return 0.25;  // average
    }
};

// Per-hand tracking (reset each hand)
struct HandTracking {
    bool we_raised_preflop = false;
    bool we_cbet_flop      = false;
    bool we_bet_turn       = false;
    int  last_raiser_seat  = -1;      // most recent raiser (any street)

    // Opponent aggression this hand
    int  total_opp_raises  = 0;
    bool opp_allin         = false;

    // VPIP / PFR tracking sets (avoid double-counting per hand)
    std::set<int> vpip_counted;
    std::set<int> pfr_counted;
    std::set<int> folded_seats;

    // Our last vote (for deduction from VOTE_RESULT)
    int  our_last_vote_amount = 0;
    bool our_last_vote_yes    = true;

    void reset() {
        we_raised_preflop = false;
        we_cbet_flop      = false;
        we_bet_turn       = false;
        last_raiser_seat  = -1;
        total_opp_raises  = 0;
        opp_allin         = false;
        vpip_counted.clear();
        pfr_counted.clear();
        folded_seats.clear();
        our_last_vote_amount = 0;
        our_last_vote_yes    = true;
    }
};

// Tracks opponent voting tendencies by deduction from VOTE_RESULT
struct VoteTracker {
    int total_opp_yes = 0;   // cumulative opponent YES wagers
    int total_opp_no  = 0;   // cumulative opponent NO wagers
    int vote_count    = 0;   // number of votes processed

    void record(int yes_total, int no_total, int our_amount, bool our_yes) {
        if (our_yes) {
            total_opp_yes += std::max(0, yes_total - our_amount);
            total_opp_no  += no_total;
        } else {
            total_opp_yes += yes_total;
            total_opp_no  += std::max(0, no_total - our_amount);
        }
        vote_count++;
    }

    // Average opponent YES wager per vote
    double avg_opp_yes_wager() const {
        return vote_count > 0 ? (double)total_opp_yes / vote_count : 0.0;
    }
    // Average opponent NO wager per vote
    double avg_opp_no_wager() const {
        return vote_count > 0 ? (double)total_opp_no / vote_count : 0.0;
    }
    // Opponent tendency: > 0.5 = opponents lean YES (keep boards)
    double opp_yes_tendency() const {
        int total = total_opp_yes + total_opp_no;
        return total > 0 ? (double)total_opp_yes / total : 0.5;
    }
};

// ---------------------------------------------------------------------------
// Preflop hand strength (heads-up approximation)
// ---------------------------------------------------------------------------

static double preflop_strength_hu(int r1, int s1, int r2, int s2) {
    int r_hi = std::max(r1, r2), r_lo = std::min(r1, r2);
    bool suited = (s1 == s2);

    if (r1 == r2)
        return 0.52 + (r_hi - 2) * (0.85 - 0.52) / 12.0;

    double base = 0.37 + (r_hi - 2) * 0.19 / 12.0 + (r_lo - 2) * 0.07 / 12.0;
    double suited_bonus = suited ? 0.030 : 0.0;
    int gap = r_hi - r_lo - 1;
    double conn = (gap <= 4) ? std::max(0.0, (4 - gap) * 0.004) : 0.0;
    double raw = base + suited_bonus + conn;

    if      (r_hi <= 9)  raw *= 0.82;
    else if (r_hi <= 11) raw *= 0.93;

    return std::min(0.70, raw);
}

// ---------------------------------------------------------------------------
// Monte Carlo win probability — SIGNIFICANTLY more sims than v1
// ---------------------------------------------------------------------------

static std::mt19937 RNG(std::random_device{}());

static int mc_sims(int street_idx) {
    switch (street_idx) {
        case 1:  return 400;   // flop
        case 2:  return 600;   // turn
        case 3:  return 1000;  // river
        default: return 250;   // preflop swap/other
    }
}

static double win_probability(
        const std::vector<Card>& hole,
        const std::vector<Card>& community,
        int num_opponents,
        int simulations = 200)
{
    if (num_opponents <= 0) return 1.0;

    // Preflop shortcut
    if (community.empty() && hole.size() == 2) {
        double base = preflop_strength_hu(hole[0].rank, hole[0].suit,
                                           hole[1].rank, hole[1].suit);
        double strength = base;
        for (int i = 1; i < num_opponents; i++) strength *= 0.82;
        return std::max(0.01, std::min(0.99, strength));
    }

    // Build remaining deck
    std::vector<Card> remaining;
    remaining.reserve(52);
    for (int i = 0; i < 52; i++) {
        const Card& c = FULL_DECK[i];
        bool known = false;
        for (auto& h : hole)      if (h == c) { known = true; break; }
        if (!known) for (auto& cm : community) if (cm == c) { known = true; break; }
        if (!known) remaining.push_back(c);
    }

    int cards_needed = 5 - (int)community.size();
    int wins = 0, ties = 0;

    Card my_cards[7];
    for (int i = 0; i < (int)hole.size(); i++) my_cards[i] = hole[i];
    Card opp_cards[7];

    for (int sim = 0; sim < simulations; sim++) {
        std::shuffle(remaining.begin(), remaining.end(), RNG);

        int idx = cards_needed;

        Card sim_comm[5];
        for (int i = 0; i < (int)community.size(); i++) sim_comm[i] = community[i];
        for (int i = 0; i < cards_needed; i++)           sim_comm[(int)community.size() + i] = remaining[i];
        int comm_total = 5;

        for (int i = 0; i < comm_total; i++) my_cards[(int)hole.size() + i] = sim_comm[i];
        int my_n = (int)hole.size() + comm_total;
        Score my_score = evaluate_cards(my_cards, my_n);

        Score best_opp = {-1, 0, 0, 0, 0, 0};
        bool has_opp = false;
        for (int o = 0; o < num_opponents; o++) {
            if (idx + 2 > (int)remaining.size()) break;
            for (int i = 0; i < comm_total; i++) opp_cards[i] = sim_comm[i];
            opp_cards[comm_total]     = remaining[idx];
            opp_cards[comm_total + 1] = remaining[idx + 1];
            idx += 2;
            Score os = evaluate_cards(opp_cards, comm_total + 2);
            if (!has_opp || os > best_opp) { best_opp = os; has_opp = true; }
        }

        if (!has_opp || my_score > best_opp) wins++;
        else if (my_score == best_opp) ties++;
    }

    return (wins + 0.5 * ties) / simulations;
}

// ---------------------------------------------------------------------------
// Game state (enhanced)
// ---------------------------------------------------------------------------

struct GameState {
    int seat        = 0;
    int num_players = 2;
    int starting_chips = 1000;
    int swap_mults[4] = {5, 15, 25, 50};

    std::vector<Card> hole_cards;
    std::vector<Card> community;
    std::vector<int>  chips;
    int my_chips    = 1000;
    int small_blind = 1;
    int num_active  = 1;   // opponents still in current hand
    int street_idx  = 0;   // 0=pre, 1=flop, 2=turn, 3=river
    int last_swap_idx    = 0;
    int swap_count_phase = 0;

    // Position flags
    bool is_dealer = false;
    bool is_sb     = false;
    bool is_bb     = false;

    // Opponent modelling (indexed by seat)
    std::vector<OpponentStats> opp_stats;

    // Per-hand tracking
    HandTracking ht;

    // Cross-hand vote intelligence
    VoteTracker vote_tracker;

    void reset_for_hand(int active_opps, bool dealer, bool sb, bool bb) {
        hole_cards.clear();
        community.clear();
        num_active  = active_opps;
        street_idx  = 0;
        swap_count_phase = 0;
        is_dealer   = dealer;
        is_sb       = sb;
        is_bb       = bb;
        ht.reset();
    }

    // Average fold rate of non-folded opponents (ONLY using opponents with real data)
    // Returns low default (0.15) when no opponent has enough data → prevents blind bluffing
    double avg_opponent_fold_rate() const {
        double sum = 0; int cnt = 0;
        for (int i = 0; i < (int)opp_stats.size(); i++) {
            if (i == seat) continue;
            if (chips[i] <= 0) continue;
            if (ht.folded_seats.count(i)) continue;
            if (!opp_stats[i].has_sufficient_data()) continue;  // skip unknowns
            sum += opp_stats[i].fold_rate();
            cnt++;
        }
        return cnt > 0 ? sum / cnt : 0.15;  // default = assume they don't fold
    }

    // Whether bluffing has a chance of working (requires evidence of folding)
    bool bluff_viable() const {
        return avg_opponent_fold_rate() > 0.40;
    }

    // Average aggression of non-folded opponents (only with sufficient data)
    double avg_opponent_aggression() const {
        double sum = 0; int cnt = 0;
        for (int i = 0; i < (int)opp_stats.size(); i++) {
            if (i == seat) continue;
            if (chips[i] <= 0) continue;
            if (ht.folded_seats.count(i)) continue;
            if (!opp_stats[i].has_sufficient_data()) continue;
            sum += opp_stats[i].aggression();
            cnt++;
        }
        return cnt > 0 ? sum / cnt : 1.0;
    }
};

// ---------------------------------------------------------------------------
// Decision: BETTING (with bluffing, semi-bluffing, opponent-aware sizing)
// ---------------------------------------------------------------------------

static std::string decide_action(GameState& st, int chips, int current_bet,
                                  int your_bet, int min_raise, int pot)
{
    int to_call = current_bet - your_bet;
    int sims = mc_sims(st.street_idx);
    double wp = win_probability(st.hole_cards, st.community, st.num_active, sims);

    // --- Position adjustment ---
    double pos_adj = st.is_dealer ? 0.06 : (st.is_bb ? 0.03 : (st.is_sb ? -0.02 : 0.0));
    double wp_eff = wp + pos_adj;

    // --- Opponent adjustment ---
    // Against a tight raiser, our equity is likely overestimated (they have a narrow strong range)
    // Against a maniac, our equity is underestimated (they raise with junk)
    if (to_call > 0 && st.ht.last_raiser_seat >= 0 &&
        st.ht.last_raiser_seat < (int)st.opp_stats.size()) {
        const auto& rs = st.opp_stats[st.ht.last_raiser_seat];
        if (rs.total_actions() > 30) {  // require strong evidence (was 15)
            if (rs.aggression() < 0.6 && rs.vpip_rate() < 0.30) {
                // Tight nit raising → they have it. Discount equity (conservative).
                wp_eff -= 0.06;
            } else if (rs.aggression() > 2.0 || rs.vpip_rate() > 0.65) {
                // Maniac / very loose → their raise means less. Boost equity.
                wp_eff += 0.05;
            }
        }
    }

    // --- Draw detection ---
    DrawInfo draws = detect_draws(st.hole_cards, st.community);

    // --- Implied odds for draws ---
    // Draws justify calling slightly beyond raw pot-odds because when we hit,
    // we win extra chips on later streets.
    double implied_bonus = 0.0;
    if (draws.total_outs > 0 && st.street_idx < 3) {
        implied_bonus = draws.total_outs * 0.005 * (4 - st.street_idx);
        // Combo draw (flush + straight) gets extra credit
        if (draws.has_combo_draw()) implied_bonus *= 1.4;
    }
    double wp_with_implied = wp_eff + implied_bonus;

    // --- Bluff roll (random number for this decision) ---
    double bluff_roll = std::uniform_real_distribution<double>(0.0, 1.0)(RNG);
    // Randomised bet sizing jitter (±10%) to be unpredictable
    double size_jitter = 0.90 + std::uniform_real_distribution<double>(0.0, 0.20)(RNG);

    int bb = st.small_blind * 2;

    // =======================================================================
    // Short-stack push/fold
    // =======================================================================
    if (bb > 0 && chips / bb <= 8) {
        double push_thresh = std::max(0.28, 0.42 - (8 - chips / bb) * 0.018);
        // With draws, modest push loosening
        if (draws.has_strong_draw()) push_thresh -= 0.04;
        if (wp_eff > push_thresh)
            return "ALLIN";
        return (to_call == 0) ? "CHECK" : "FOLD";
    }

    // =======================================================================
    // Facing all-in (must call full stack or fold)
    // =======================================================================
    if (to_call >= chips) {
        double threshold = 0.45;
        // Loosen vs confirmed maniacs only
        if (st.avg_opponent_aggression() > 2.0) threshold -= 0.04;
        // Draws add modest equity
        if (draws.has_strong_draw() && st.street_idx < 3) threshold -= 0.04;
        return (wp_with_implied > threshold) ? "ALLIN" : "FOLD";
    }

    // =======================================================================
    // No bet to face — can check or bet
    // =======================================================================
    if (to_call == 0) {
        // -- VALUE BETS --
        if (wp_eff > 0.85) {
            // Monster: pot-sized bet
            int raise_amt = std::min(chips, std::max(min_raise, (int)(pot * size_jitter)));
            if (raise_amt >= min_raise)
                return "RAISE " + std::to_string(raise_amt);
        }
        if (wp_eff > 0.68) {
            // Strong hand: 55–65% pot
            int raise_amt = std::min(chips, std::max(min_raise, (int)(pot * 0.60 * size_jitter)));
            if (raise_amt >= min_raise)
                return "RAISE " + std::to_string(raise_amt);
        }
        if (wp_eff > 0.55 && st.is_dealer) {
            // In position with decent hand: 33–40% pot probe
            int raise_amt = std::min(chips, std::max(min_raise, (int)(pot * 0.37 * size_jitter)));
            if (raise_amt >= min_raise)
                return "RAISE " + std::to_string(raise_amt);
        }

        // ==============================================================
        // BLUFFING — only when opponent history proves they fold enough
        //
        // Philosophy: NEVER bluff blind. Every bluff requires evidence.
        //   • No data        → play value only (no chip waste)
        //   • Calling station → play value only (they don't fold)
        //   • Foldy opponent  → bluffs become +EV, unlock them
        //   EXCEPTION: combo draws (15+ outs, ~55% equity on flop)
        //   are essentially value bets and always fire.
        // ==============================================================

        bool bluff_ok = st.bluff_viable();   // fold_rate > 0.40 with real data
        double opp_fold = st.avg_opponent_fold_rate();

        // -- SEMI-BLUFFS (draws: combo draws always fire, others need evidence) --
        if (draws.has_strong_draw() && wp_eff < 0.55 && wp_eff > 0.15 && st.street_idx >= 1) {
            // Combo draws (~55% equity on flop) are VALUE bets, not really bluffs
            if (draws.has_combo_draw()) {
                double semi_freq = 0.65;
                if (st.is_dealer) semi_freq += 0.08;
                if (bluff_roll < semi_freq) {
                    int raise_amt = std::min(chips, std::max(min_raise, (int)(pot * 0.60 * size_jitter)));
                    if (raise_amt >= min_raise)
                        return "RAISE " + std::to_string(raise_amt);
                }
            }
            // Regular flush/OESD draw: only semi-bluff if opponents fold enough
            else if (bluff_ok) {
                double semi_freq = std::max(0.0, (opp_fold - 0.35) * 0.80);
                if (st.is_dealer) semi_freq += 0.06;
                semi_freq = std::min(0.45, semi_freq);
                if (bluff_roll < semi_freq) {
                    int raise_amt = std::min(chips, std::max(min_raise, (int)(pot * 0.60 * size_jitter)));
                    if (raise_amt >= min_raise)
                        return "RAISE " + std::to_string(raise_amt);
                }
            }
        }

        // -- C-BET — ONLY if we have evidence opponents fold to bets --
        if (bluff_ok && st.ht.we_raised_preflop && st.street_idx == 1 &&
            wp_eff < 0.55 && wp_eff > 0.12) {
            double wetness = board_wetness(st.community);
            // Frequency scales with opponent fold rate; dry boards = higher
            double cbet_freq = std::max(0.0, (opp_fold - 0.35) * 1.2) * (1.0 - wetness * 0.4);
            cbet_freq = std::min(0.55, cbet_freq);
            if (bluff_roll < cbet_freq) {
                int raise_amt = std::min(chips, std::max(min_raise, (int)(pot * 0.50 * size_jitter)));
                if (raise_amt >= min_raise)
                    return "RAISE " + std::to_string(raise_amt);
            }
        }

        // -- DOUBLE BARREL — strict evidence required --
        if (bluff_ok && st.ht.we_cbet_flop && st.street_idx == 2 &&
            wp_eff < 0.55 && wp_eff > 0.18) {
            double barrel_freq = std::max(0.0, (opp_fold - 0.40) * 0.80);
            if (draws.has_any_draw()) barrel_freq += 0.10;
            barrel_freq = std::min(0.40, barrel_freq);
            if (bluff_roll < barrel_freq) {
                int raise_amt = std::min(chips, std::max(min_raise, (int)(pot * 0.55 * size_jitter)));
                if (raise_amt >= min_raise)
                    return "RAISE " + std::to_string(raise_amt);
            }
        }

        // -- PURE POSITION BLUFF — strongest evidence required --
        if (bluff_ok && st.is_dealer && wp_eff < 0.45 && wp_eff > 0.15 && st.street_idx >= 1) {
            double bluff_freq = std::max(0.0, (opp_fold - 0.48) * 0.50);
            bluff_freq = std::min(0.18, bluff_freq);
            if (bluff_roll < bluff_freq) {
                int raise_amt = std::min(chips, std::max(min_raise, (int)(pot * 0.55 * size_jitter)));
                if (raise_amt >= min_raise)
                    return "RAISE " + std::to_string(raise_amt);
            }
        }

        return "CHECK";
    }

    // =======================================================================
    // Facing a bet — decide call / raise / fold
    // =======================================================================

    double pot_after = pot + to_call;
    double pot_odds  = (pot_after > 0) ? (to_call / pot_after) : 0.0;

    // -- VALUE RE-RAISES --
    if (wp_eff > 0.85) {
        // Monster: large re-raise (pot + call)
        int raise_amt = std::min(chips + your_bet, std::max(min_raise, (int)((pot + to_call) * size_jitter)));
        if (raise_amt >= min_raise)
            return "RAISE " + std::to_string(raise_amt);
        return "CALL";
    }
    if (wp_eff > 0.73) {
        // Strong: raise 70% pot
        int raise_amt = std::min(chips + your_bet, std::max(min_raise, (int)(pot * 0.70 * size_jitter)));
        if (raise_amt >= min_raise)
            return "RAISE " + std::to_string(raise_amt);
        return "CALL";
    }

    // -- SEMI-BLUFF RAISE — combo draws always, others need fold evidence --
    if (draws.has_strong_draw() && wp_eff > 0.20 && st.street_idx < 3) {
        if (draws.has_combo_draw()) {
            // Combo draw raise (~value, not really a bluff)
            double semi_raise_freq = 0.35;
            if (bluff_roll < semi_raise_freq) {
                int raise_amt = std::min(chips + your_bet, std::max(min_raise, (int)(pot * 0.70 * size_jitter)));
                if (raise_amt >= min_raise)
                    return "RAISE " + std::to_string(raise_amt);
            }
        } else if (st.bluff_viable()) {
            // Regular draw raise only vs known folders
            double opp_fold = st.avg_opponent_fold_rate();
            double semi_raise_freq = std::max(0.0, (opp_fold - 0.40) * 0.50);
            semi_raise_freq = std::min(0.30, semi_raise_freq);
            if (bluff_roll < semi_raise_freq) {
                int raise_amt = std::min(chips + your_bet, std::max(min_raise, (int)(pot * 0.70 * size_jitter)));
                if (raise_amt >= min_raise)
                    return "RAISE " + std::to_string(raise_amt);
            }
        }
    }

    // -- CALL DECISIONS (pot-odds + implied odds + opponent adjustment) --
    if (wp_with_implied > pot_odds * 1.05)
        return "CALL";

    // Marginal call: call small bets (< 10% stack) with decent equity
    if (wp_with_implied > pot_odds * 0.80 && to_call <= chips / 10)
        return "CALL";

    // ===================================================================
    // BLUFF CATCHING: use opponent's aggression-vs-showdown profile
    // to identify likely bluffers and call them down wider.
    //
    // Key insight: if an opponent raises a lot but shows weak hands
    // at showdown, they're bluffing frequently → we need less equity
    // to call profitably (their bet represents air more often).
    // ===================================================================
    if (st.ht.last_raiser_seat >= 0 && st.ht.last_raiser_seat < (int)st.opp_stats.size()) {
        const auto& rs = st.opp_stats[st.ht.last_raiser_seat];
        double bluff_prob = rs.estimate_bluff_frequency();

        // Against confirmed bluffers: widen calling range significantly
        if (bluff_prob > 0.35 && rs.has_sufficient_data()) {
            double bluff_call_bonus = (bluff_prob - 0.20) * 0.40;
            if (wp_eff + bluff_call_bonus > pot_odds * 0.85)
                return "CALL";
        }

        // Legacy check: very aggressive opponents (even without showdown data)
        if (rs.total_actions() > 30 && rs.aggression() > 2.0) {
            if (wp_eff > pot_odds * 0.70)
                return "CALL";
        }
    }

    // -- BLUFF RE-RAISE — very rare, strongest fold evidence required --
    if (st.bluff_viable() && st.is_dealer && wp_eff < 0.35 && wp_eff > 0.15 && to_call < pot / 3) {
        double opp_fold = st.avg_opponent_fold_rate();
        double reraise_freq = std::max(0.0, (opp_fold - 0.50) * 0.25);
        reraise_freq = std::min(0.08, reraise_freq);
        if (bluff_roll < reraise_freq) {
            int raise_amt = std::min(chips + your_bet, std::max(min_raise, (int)(pot * 0.85 * size_jitter)));
            if (raise_amt >= min_raise)
                return "RAISE " + std::to_string(raise_amt);
        }
    }

    return "FOLD";
}

// ---------------------------------------------------------------------------
// Decision: SWAP (draw-aware, board-texture-aware)
// ---------------------------------------------------------------------------

static std::string decide_swap(GameState& st, int cost, int my_chips,
                                int swap_num, int samples = 15) {  // 15 samples for speed
    if (cost > my_chips * 0.30) return "STAY";
    if ((int)st.hole_cards.size() < 2) return "STAY";

    // ===============================================================
    // PRE-FLOP HAND PRESERVATION: never swap structurally good hands
    // ===============================================================
    if (st.community.empty()) {
        int r1 = st.hole_cards[0].rank, r2 = st.hole_cards[1].rank;
        int s1 = st.hole_cards[0].suit, s2 = st.hole_cards[1].suit;
        int hi = std::max(r1, r2), lo = std::min(r1, r2);
        bool suited = (s1 == s2);
        int gap = hi - lo;

        // Pocket pairs: ALWAYS keep (even small ones have ~50%+ equity HU)
        if (r1 == r2) return "STAY";
        // Premium hands: both 10+
        if (hi >= 10 && lo >= 10) return "STAY";
        // Suited connectors/one-gappers: keep for flush+straight potential
        if (suited && gap <= 3) return "STAY";
        // Ace-x suited: keep for nut flush potential
        if (suited && hi == 14) return "STAY";
    }

    int sims = std::min(80, mc_sims(st.street_idx));  // capped for swap speed
    double cur = win_probability(st.hole_cards, st.community, st.num_active, sims);

    // ---- Draw protection: don't swap away a card that's part of a strong draw ----
    DrawInfo draws = detect_draws(st.hole_cards, st.community);
    bool protect_card[2] = {false, false};

    if (draws.has_flush_draw && draws.flush_suit >= 0) {
        for (int i = 0; i < 2; i++)
            if (st.hole_cards[i].suit == draws.flush_suit) protect_card[i] = true;
    }
    if (draws.has_oesd || draws.has_gutshot) {
        if (draws.has_combo_draw()) {
            protect_card[0] = protect_card[1] = true;
        }
    }

    // ---- Board-matching protection (post-flop) ----
    // If a hole card pairs the board → potential trips/full house, protect it
    if (!st.community.empty()) {
        for (int i = 0; i < 2; i++) {
            for (auto& cm : st.community) {
                if (st.hole_cards[i].rank == cm.rank) {
                    protect_card[i] = true;  // pairs the board
                    break;
                }
            }
        }
    }

    if (protect_card[0] && protect_card[1]) return "STAY";
    if (cur > 0.70) return "STAY";

    // Build remaining deck
    std::vector<Card> remaining;
    for (int i = 0; i < 52; i++) {
        const Card& c = FULL_DECK[i];
        bool known = false;
        for (auto& h : st.hole_cards) if (h == c) { known = true; break; }
        if (!known) for (auto& cm : st.community) if (cm == c) { known = true; break; }
        if (!known) remaining.push_back(c);
    }
    if ((int)remaining.size() < samples) return "STAY";

    std::shuffle(remaining.begin(), remaining.end(), RNG);
    std::vector<Card> sample(remaining.begin(), remaining.begin() + samples);

    double best_improvement = 0.0;
    int best_i = -1;

    for (int i = 0; i < 2; i++) {
        if (protect_card[i]) continue;  // skip protected cards

        Card other = st.hole_cards[1 - i];
        double total = 0.0;
        for (auto& rep : sample) {
            std::vector<Card> test_hole = {other, rep};
            total += win_probability(test_hole, st.community, st.num_active, 8);
        }
        double avg = total / samples;
        double improvement = avg - cur;
        if (improvement > best_improvement) {
            best_improvement = improvement;
            best_i = i;
        }
    }

    // Threshold: weak hands need less absolute improvement to justify a swap
    double base_thresh  = std::max(0.03, cur * 0.10);
    double swap_thresh  = base_thresh * (swap_num == 0 ? 1.0 : 2.5);
    double cost_limit   = (swap_num == 0) ? 0.15 : 0.08;

    if (best_i >= 0 && best_improvement > swap_thresh && (double)cost / my_chips <= cost_limit) {
        return "SWAP " + std::to_string(best_i);
    }
    return "STAY";
}

// ---------------------------------------------------------------------------
// Redraw equity estimation (unchanged core, more sims)
// ---------------------------------------------------------------------------

static double estimate_redraw_equity(GameState& st, int board_cards_kept, int simulations = 15) {
    int cards_to_redraw = (int)st.community.size() - board_cards_kept;
    if (cards_to_redraw <= 0)
        return win_probability(st.hole_cards, st.community, st.num_active, simulations);

    std::vector<Card> remaining;
    remaining.reserve(52);
    for (int i = 0; i < 52; i++) {
        const Card& c = FULL_DECK[i];
        bool known = false;
        for (auto& h : st.hole_cards) if (h == c) { known = true; break; }
        if (!known) for (int j = 0; j < board_cards_kept; j++)
            if (st.community[j] == c) { known = true; break; }
        if (!known) remaining.push_back(c);
    }

    double total = 0.0;
    for (int s = 0; s < simulations; s++) {
        std::shuffle(remaining.begin(), remaining.end(), RNG);
        std::vector<Card> sim_comm(st.community.begin(),
                                   st.community.begin() + board_cards_kept);
        for (int i = 0; i < cards_to_redraw; i++) sim_comm.push_back(remaining[i]);
        total += win_probability(st.hole_cards, sim_comm, st.num_active, 12);
    }
    return total / simulations;
}

// ---------------------------------------------------------------------------
// Decision: VOTE (opponent disruption + vote history intelligence)
// ---------------------------------------------------------------------------

static std::string decide_vote(GameState& st, int my_chips) {
    if (st.community.empty()) {
        st.ht.our_last_vote_amount = 0;
        st.ht.our_last_vote_yes = true;
        return "VOTE YES 0";
    }

    int sims = mc_sims(st.street_idx);
    double cur_wp = win_probability(st.hole_cards, st.community, st.num_active, sims);

    int board_cards_kept = (int)st.community.size() - (st.street_idx == 1 ? 3 : 1);
    board_cards_kept = std::max(0, board_cards_kept);

    double redraw_wp = estimate_redraw_equity(st, board_cards_kept, 15);
    double edge = cur_wp - redraw_wp;

    // --- Draw awareness: if we have a strong draw on this board, keep it ---
    DrawInfo draws = detect_draws(st.hole_cards, st.community);
    if (draws.has_strong_draw()) {
        edge += 0.05;
    }

    // ===================================================================
    // OPPONENT DISRUPTION: if opponents have been aggressive this hand,
    // they likely love this board → vote NO to blow it up.
    // ===================================================================
    double disruption_bias = 0.0;
    if (st.ht.total_opp_raises >= 2) {
        disruption_bias = -0.12;
    } else if (st.ht.total_opp_raises >= 1) {
        disruption_bias = -0.06;
    }
    if (st.ht.opp_allin) {
        disruption_bias = -0.18;
    }
    // Don't disrupt our own strong board
    if (cur_wp > 0.65) {
        disruption_bias = 0.0;
    }

    edge += disruption_bias;

    // ===================================================================
    // CHIP POSITION AWARENESS: if we're behind (short-stacked), we
    // benefit from chaos/variance. If we're ahead, preserve stability.
    // ===================================================================
    bool we_are_chip_leader = true;
    bool we_are_short_stack = true;
    for (int i = 0; i < (int)st.chips.size(); i++) {
        if (i == st.seat || st.chips[i] <= 0) continue;
        if (st.chips[i] >= st.my_chips) we_are_chip_leader = false;
        if (st.chips[i] <= st.my_chips) we_are_short_stack = false;
    }
    // Short-stacked: welcome chaos (slight NO bias for variance)
    if (we_are_short_stack && cur_wp < 0.50) edge -= 0.04;
    // Chip leader: prefer stability (slight YES bias)
    if (we_are_chip_leader && cur_wp > 0.40) edge += 0.03;

    // --- Wager sizing ---
    double confidence = std::abs(edge);
    double max_wager_pct = 0.18;
    if (confidence > 0.30) max_wager_pct = 0.25;

    // STRONG HAND PROTECTION: when we have a monster on this board, the
    // wager goes into the pot which we're highly likely to win → effective
    // cost is very low.  Wager big to prevent opponents redrawing our board.
    if (cur_wp > 0.80 && edge > 0.15) {
        max_wager_pct = 0.35;   // monster: protect hard
        confidence = std::max(confidence, 0.50);  // ensure high wager
    } else if (cur_wp > 0.65 && edge > 0.10) {
        max_wager_pct = std::max(max_wager_pct, 0.28);  // strong: protect
        confidence = std::max(confidence, 0.35);
    }

    int wager = (int)(my_chips * std::min(max_wager_pct, confidence * 0.45));
    wager = std::max(0, wager);

    // --- SMART WAGER: use vote history to outspend opponents ---
    // If we want to swing the vote, we need to outspend the opponent pool
    bool vote_yes = (edge >= 0);
    if (st.vote_tracker.vote_count >= 2) {
        if (vote_yes) {
            // We want YES; opponents historically wager avg_opp_no on NO
            double avg_opp_no = st.vote_tracker.avg_opp_no_wager();
            if (avg_opp_no > wager && confidence > 0.10) {
                // Budget enough to beat their expected NO votes
                wager = std::min((int)(avg_opp_no * 1.3 + 1), (int)(my_chips * max_wager_pct));
            }
        } else {
            // We want NO; opponents historically wager avg_opp_yes on YES
            double avg_opp_yes = st.vote_tracker.avg_opp_yes_wager();
            if (avg_opp_yes > wager && confidence > 0.10) {
                wager = std::min((int)(avg_opp_yes * 1.3 + 1), (int)(my_chips * max_wager_pct));
            }
        }
    }
    wager = std::max(0, wager);

    // --- DISRUPTION: we're behind AND opponents are betting big ---
    if (cur_wp < 0.35 && st.ht.total_opp_raises >= 2 && st.street_idx >= 1) {
        wager = std::max(wager, (int)(my_chips * 0.20));
        st.ht.our_last_vote_amount = std::min(wager, my_chips);
        st.ht.our_last_vote_yes = false;
        return "VOTE NO " + std::to_string(std::min(wager, my_chips));
    }

    // --- Record our vote for deduction ---
    st.ht.our_last_vote_amount = wager;
    st.ht.our_last_vote_yes = vote_yes;

    if (vote_yes)
        return "VOTE YES " + std::to_string(wager);
    return "VOTE NO " + std::to_string(wager);
}

// ---------------------------------------------------------------------------
// Message loop (enhanced: processes SHOWDOWN, WINNER, opponent actions)
// ---------------------------------------------------------------------------

int main() {
    std::cout << std::unitbuf;
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    init_deck();
    GameState st;

    double max_time_ms = 0.0;
    int decision_count = 0;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        // ----------------------------------------------------------------
        // GAME_START
        // ----------------------------------------------------------------
        if (cmd == "GAME_START") {
            int np, seat, sc, m0, m1, m2, m3;
            iss >> np >> seat >> sc >> m0 >> m1 >> m2 >> m3;
            st.num_players    = np;
            st.seat           = seat;
            st.starting_chips = sc;
            st.swap_mults[0]  = m0;
            st.swap_mults[1]  = m1;
            st.swap_mults[2]  = m2;
            st.swap_mults[3]  = m3;
            st.my_chips       = sc;
            st.chips.assign(np, sc);
            st.opp_stats.resize(np);

        // ----------------------------------------------------------------
        // HAND_START
        // ----------------------------------------------------------------
        } else if (cmd == "HAND_START") {
            int hn, dealer, sb_s, bb_s, sba, bba;
            iss >> hn >> dealer >> sb_s >> bb_s >> sba >> bba;
            st.small_blind = sba;

            int active = 0;
            for (int i = 0; i < (int)st.chips.size(); i++) {
                if (i != st.seat && st.chips[i] > 0) {
                    active++;
                    st.opp_stats[i].hands_seen++;
                }
            }
            st.reset_for_hand(active,
                dealer == st.seat,
                sb_s   == st.seat,
                bb_s   == st.seat);

        // ----------------------------------------------------------------
        // CHIPS
        // ----------------------------------------------------------------
        } else if (cmd == "CHIPS") {
            st.chips.clear();
            int c;
            while (iss >> c) st.chips.push_back(c);
            if (st.seat < (int)st.chips.size())
                st.my_chips = st.chips[st.seat];

        // ----------------------------------------------------------------
        // DEAL_HOLE
        // ----------------------------------------------------------------
        } else if (cmd == "DEAL_HOLE") {
            std::string c1, c2;
            iss >> c1 >> c2;
            st.hole_cards = {parse_card(c1), parse_card(c2)};

        // ----------------------------------------------------------------
        // Community cards
        // ----------------------------------------------------------------
        } else if (cmd == "DEAL_FLOP") {
            std::string c1, c2, c3;
            iss >> c1 >> c2 >> c3;
            st.community = {parse_card(c1), parse_card(c2), parse_card(c3)};
            st.street_idx = 1;
            st.swap_count_phase = 0;

        } else if (cmd == "DEAL_TURN") {
            std::string c1; iss >> c1;
            st.community.push_back(parse_card(c1));
            st.street_idx = 2;
            st.swap_count_phase = 0;

        } else if (cmd == "DEAL_RIVER") {
            std::string c1; iss >> c1;
            st.community.push_back(parse_card(c1));
            st.street_idx = 3;
            st.swap_count_phase = 0;

        // ----------------------------------------------------------------
        // SWAP_RESULT
        // ----------------------------------------------------------------
        } else if (cmd == "SWAP_RESULT") {
            std::string c1; iss >> c1;
            if (st.last_swap_idx < (int)st.hole_cards.size())
                st.hole_cards[st.last_swap_idx] = parse_card(c1);

        // ----------------------------------------------------------------
        // Redraws
        // ----------------------------------------------------------------
        } else if (cmd == "REDRAW_FLOP") {
            std::string c1, c2, c3; iss >> c1 >> c2 >> c3;
            if ((int)st.community.size() >= 3) {
                st.community[0] = parse_card(c1);
                st.community[1] = parse_card(c2);
                st.community[2] = parse_card(c3);
            } else {
                st.community = {parse_card(c1), parse_card(c2), parse_card(c3)};
            }
        } else if (cmd == "REDRAW_TURN") {
            std::string c1; iss >> c1;
            if ((int)st.community.size() >= 4) st.community[3] = parse_card(c1);
            else st.community.push_back(parse_card(c1));
        } else if (cmd == "REDRAW_RIVER") {
            std::string c1; iss >> c1;
            if ((int)st.community.size() >= 5) st.community[4] = parse_card(c1);
            else st.community.push_back(parse_card(c1));

        // ----------------------------------------------------------------
        // ACTION — track all opponent actions for modelling
        // ----------------------------------------------------------------
        } else if (cmd == "ACTION") {
            int seat; std::string action;
            iss >> seat >> action;
            int amount = 0;
            iss >> amount;  // may or may not be present

            if (seat == st.seat) {
                // --- Track our own actions for multi-street planning ---
                if (action == "RAISE" || action == "ALLIN") {
                    if (st.street_idx == 0)
                        st.ht.we_raised_preflop = true;
                    else if (st.street_idx == 1 && st.ht.we_raised_preflop)
                        st.ht.we_cbet_flop = true;
                    else if (st.street_idx == 2 && st.ht.we_cbet_flop)
                        st.ht.we_bet_turn = true;
                }
            } else {
                // --- Opponent action tracking ---
                if (action == "FOLD") {
                    st.num_active = std::max(0, st.num_active - 1);
                    st.ht.folded_seats.insert(seat);
                }

                if (seat < (int)st.opp_stats.size()) {
                    auto& os = st.opp_stats[seat];

                    if (action == "FOLD")       os.fold_count++;
                    else if (action == "CHECK")  os.check_count++;
                    else if (action == "CALL")   os.call_count++;
                    else if (action == "RAISE" || action == "ALLIN")
                                                 os.raise_count++;

                    // VPIP: preflop voluntary action (call/raise/allin, not check/fold)
                    if (st.street_idx == 0 &&
                        (action == "CALL" || action == "RAISE" || action == "ALLIN") &&
                        st.ht.vpip_counted.find(seat) == st.ht.vpip_counted.end()) {
                        st.ht.vpip_counted.insert(seat);
                        os.vpip_count++;
                    }

                    // PFR: preflop raise/allin
                    if (st.street_idx == 0 &&
                        (action == "RAISE" || action == "ALLIN") &&
                        st.ht.pfr_counted.find(seat) == st.ht.pfr_counted.end()) {
                        st.ht.pfr_counted.insert(seat);
                        os.pfr_count++;
                    }
                }

                // Track raises this hand (for vote disruption)
                if (action == "RAISE" || action == "ALLIN") {
                    st.ht.total_opp_raises++;
                    st.ht.last_raiser_seat = seat;
                }
                if (action == "ALLIN") {
                    st.ht.opp_allin = true;
                }
            }

        // ----------------------------------------------------------------
        // SHOWDOWN — harvest opponent hand data!
        // ----------------------------------------------------------------
        } else if (cmd == "SHOWDOWN") {
            int seat;
            std::string c1, c2;
            iss >> seat >> c1 >> c2;

            if (seat != st.seat && seat < (int)st.opp_stats.size()) {
                st.opp_stats[seat].showdown_count++;

                // Evaluate their actual hand strength against the board
                if ((int)st.community.size() >= 5) {
                    Card opp_cards[7];
                    for (int i = 0; i < 5; i++) opp_cards[i] = st.community[i];
                    opp_cards[5] = parse_card(c1);
                    opp_cards[6] = parse_card(c2);
                    Score opp_score = evaluate_cards(opp_cards, 7);
                    st.opp_stats[seat].showdown_hand_types.push_back(opp_score[0]);
                }
            }

        // ----------------------------------------------------------------
        // WINNER — track who wins and how often
        // ----------------------------------------------------------------
        } else if (cmd == "WINNER") {
            int seat, amount;
            std::string rank;
            iss >> seat >> amount >> rank;

            if (seat != st.seat && seat < (int)st.opp_stats.size()) {
                st.opp_stats[seat].wins++;
            }

        // ----------------------------------------------------------------
        // ELIMINATE
        // ----------------------------------------------------------------
        } else if (cmd == "ELIMINATE") {
            int seat; iss >> seat;
            if (seat < (int)st.chips.size()) st.chips[seat] = 0;

        } else if (cmd == "GAME_OVER") {
            break;

        // ================================================================
        // PROMPTS — must respond immediately
        // ================================================================

        } else if (cmd == "ACTION_PROMPT") {
            auto t0 = std::chrono::steady_clock::now();
            int chips, cb, yb, mr, pot;
            iss >> chips >> cb >> yb >> mr >> pot;
            st.my_chips = chips;
            std::cout << decide_action(st, chips, cb, yb, mr, pot) << "\n";
            auto t1 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            if (ms > max_time_ms) max_time_ms = ms;
            decision_count++;

        } else if (cmd == "SWAP_PROMPT") {
            auto t0 = std::chrono::steady_clock::now();
            int cost, chips;
            iss >> cost >> chips;
            st.my_chips = chips;
            if (st.swap_count_phase >= 2) {
                std::cout << "STAY\n";
            } else {
                std::string resp = decide_swap(st, cost, chips, st.swap_count_phase);
                if (resp.rfind("SWAP ", 0) == 0) {
                    st.last_swap_idx = resp[5] - '0';
                    st.swap_count_phase++;
                }
                std::cout << resp << "\n";
            }
            auto t1 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            if (ms > max_time_ms) max_time_ms = ms;
            decision_count++;

        } else if (cmd == "VOTE_PROMPT") {
            auto t0 = std::chrono::steady_clock::now();
            int chips; iss >> chips;
            st.my_chips = chips;
            std::cout << decide_vote(st, chips) << "\n";
            auto t1 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            if (ms > max_time_ms) max_time_ms = ms;
            decision_count++;

        // ----------------------------------------------------------------
        // VOTE_RESULT — deduce opponent voting tendencies
        // ----------------------------------------------------------------
        } else if (cmd == "VOTE_RESULT") {
            int yes_total, no_total;
            std::string outcome;
            iss >> yes_total >> no_total >> outcome;
            st.vote_tracker.record(yes_total, no_total,
                                   st.ht.our_last_vote_amount,
                                   st.ht.our_last_vote_yes);

        // All other messages (SWAP_DONE, etc.) → ignored
        }
    }
    // Report timing to stderr (invisible to engine, visible to us)
    std::cerr << "[TIMING] decisions=" << decision_count
              << " max=" << max_time_ms << "ms" << std::endl;
    return 0;
}
