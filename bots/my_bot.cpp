/*
 * Chaos Poker bot — C++ implementation
 *
 * Strategy:
 *  - ACTION:  pot-odds + Monte Carlo win probability
 *  - SWAP:    estimate hand improvement from random card replacement vs cost
 *  - VOTE:    compare current board strength to random-board baseline
 *
 * C++ is used (instead of Python) so the process starts in microseconds,
 * never timing out on the engine's 10ms per-prompt deadline.
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
#include <cstring>
#include <cmath>

// ---------------------------------------------------------------------------
// Card
// ---------------------------------------------------------------------------

struct Card {
    int rank;  // 2–14
    int suit;  // 0–3  (s=0, h=1, d=2, c=3)
    bool operator==(const Card& o) const { return rank == o.rank && suit == o.suit; }
    bool operator<(const Card& o) const {
        return rank < o.rank || (rank == o.rank && suit < o.suit);
    }
};

Card parse_card(const std::string& s) {
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

// Full 52-card deck (built once at startup)
static Card FULL_DECK[52];
static void init_deck() {
    int i = 0;
    for (int r = 2; r <= 14; r++)
        for (int s = 0; s < 4; s++)
            FULL_DECK[i++] = {r, s};
}

// ---------------------------------------------------------------------------
// Hand evaluator — evaluate best 5-card hand from N cards (5–7)
//
// Returns an array<int,6> where [0] is hand type:
//   8=straight flush, 7=quads, 6=full house, 5=flush,
//   4=straight, 3=trips, 2=two pair, 1=pair, 0=high card
// Elements [1..5] are kickers (big-endian), 0 = unused.
// Arrays compare lexicographically, so higher is stronger.
// ---------------------------------------------------------------------------

using Score = std::array<int, 6>;

// Find highest straight top-card in a set of unique ranks.
// rank_set must contain all unique ranks; has_ace = whether 14 is present.
static int best_straight(const std::set<int>& rank_set, bool has_ace) {
    // Try 14-high down to 6-high
    for (int top = 14; top >= 6; top--) {
        bool ok = true;
        for (int i = 0; i < 5; i++) {
            if (!rank_set.count(top - i)) { ok = false; break; }
        }
        if (ok) return top;
    }
    // Wheel: A-2-3-4-5 (top = 5)
    if (has_ace) {
        bool ok = rank_set.count(2) && rank_set.count(3) &&
                  rank_set.count(4) && rank_set.count(5);
        if (ok) return 5;
    }
    return 0;
}

static Score evaluate_cards(const Card* cards, int n) {
    // Tally ranks and suits
    int rank_cnt[15] = {};
    int suit_cnt[4]  = {};
    for (int i = 0; i < n; i++) {
        rank_cnt[cards[i].rank]++;
        suit_cnt[cards[i].suit]++;
    }

    bool has_ace = (rank_cnt[14] > 0);

    // Unique ranks descending
    std::vector<int> rank_desc;
    rank_desc.reserve(13);
    for (int r = 14; r >= 2; r--)
        if (rank_cnt[r]) rank_desc.push_back(r);

    std::set<int> rank_set(rank_desc.begin(), rank_desc.end());
    int str_high = best_straight(rank_set, has_ace);

    // Check for flush suit
    int flush_suit = -1;
    for (int s = 0; s < 4; s++) {
        if (suit_cnt[s] >= 5) { flush_suit = s; break; }
    }

    if (flush_suit >= 0) {
        // Collect flush-suit ranks descending
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

        // Regular flush — take top 5
        while ((int)fr.size() > 5) fr.pop_back();
        while ((int)fr.size() < 5) fr.push_back(0);
        return {5, fr[0], fr[1], fr[2], fr[3], fr[4]};
    }

    // Build groups: sorted by (freq desc, rank desc)
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

    // Quads
    if (g0 == 4) {
        int kicker = (groups.size() > 1) ? r1 : 0;
        return {7, r0, kicker, 0, 0, 0};
    }

    // Full house
    if (g0 == 3 && g1 >= 2) {
        return {6, r0, r1, 0, 0, 0};
    }

    // Straight
    if (str_high) {
        return {4, str_high, 0, 0, 0, 0};
    }

    // Trips
    if (g0 == 3) {
        std::vector<int> k;
        for (auto& g : groups) if (g.rank != r0) k.push_back(g.rank);
        std::sort(k.rbegin(), k.rend());
        while ((int)k.size() < 2) k.push_back(0);
        return {3, r0, k[0], k[1], 0, 0};
    }

    // Two pair
    if (g0 == 2 && g1 == 2) {
        int p1 = std::max(r0, r1), p2 = std::min(r0, r1);
        int kicker = 0;
        for (auto& g : groups) if (g.rank != r0 && g.rank != r1) { kicker = g.rank; break; }
        return {2, p1, p2, kicker, 0, 0};
    }

    // One pair
    if (g0 == 2) {
        std::vector<int> k;
        for (auto& g : groups) if (g.rank != r0) k.push_back(g.rank);
        std::sort(k.rbegin(), k.rend());
        while ((int)k.size() < 3) k.push_back(0);
        return {1, r0, k[0], k[1], k[2], 0};
    }

    // High card
    std::vector<int> all;
    for (auto& g : groups) all.push_back(g.rank);
    std::sort(all.rbegin(), all.rend());
    while ((int)all.size() < 5) all.push_back(0);
    return {0, all[0], all[1], all[2], all[3], all[4]};
}

// ---------------------------------------------------------------------------
// Preflop hand strength (heads-up approximation, no community cards)
// ---------------------------------------------------------------------------

static double preflop_strength_hu(int r1, int s1, int r2, int s2) {
    int r_hi = std::max(r1, r2), r_lo = std::min(r1, r2);
    bool suited = (s1 == s2);

    if (r1 == r2) {
        // Pocket pairs: 22≈0.52, AA≈0.85
        return 0.52 + (r_hi - 2) * (0.85 - 0.52) / 12.0;
    }

    double base = 0.37 + (r_hi - 2) * 0.19 / 12.0 + (r_lo - 2) * 0.07 / 12.0;
    double suited_bonus = suited ? 0.030 : 0.0;
    int gap = r_hi - r_lo - 1;
    double conn = (gap <= 4) ? std::max(0.0, (4 - gap) * 0.004) : 0.0;
    double raw = base + suited_bonus + conn;

    // Penalise weak high-card hands
    if      (r_hi <= 9)  raw *= 0.82;
    else if (r_hi <= 11) raw *= 0.93;

    return std::min(0.70, raw);
}

// ---------------------------------------------------------------------------
// Monte Carlo win probability
// ---------------------------------------------------------------------------

static std::mt19937 RNG(std::random_device{}());

static double win_probability(
        const std::vector<Card>& hole,
        const std::vector<Card>& community,
        int num_opponents,
        int simulations = 50)
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
    remaining.reserve(52 - (int)hole.size() - (int)community.size());
    for (int i = 0; i < 52; i++) {
        const Card& c = FULL_DECK[i];
        bool known = false;
        for (auto& h : hole)      if (h == c) { known = true; break; }
        if (!known) for (auto& cm : community) if (cm == c) { known = true; break; }
        if (!known) remaining.push_back(c);
    }

    int cards_needed = 5 - (int)community.size();
    int wins = 0, ties = 0;

    // Precompute my hole + up to 5 community cards buffer
    Card my_cards[7];
    for (int i = 0; i < (int)hole.size(); i++) my_cards[i] = hole[i];
    Card opp_cards[7];

    for (int sim = 0; sim < simulations; sim++) {
        // Shuffle remaining
        std::shuffle(remaining.begin(), remaining.end(), RNG);

        int idx = cards_needed;

        // Fill community
        Card sim_comm[5];
        for (int i = 0; i < (int)community.size(); i++) sim_comm[i] = community[i];
        for (int i = 0; i < cards_needed; i++)           sim_comm[(int)community.size() + i] = remaining[i];
        int comm_total = 5;

        // My 7-card hand
        for (int i = 0; i < comm_total; i++) my_cards[(int)hole.size() + i] = sim_comm[i];
        int my_n = (int)hole.size() + comm_total;
        Score my_score = evaluate_cards(my_cards, my_n);

        // Opponents
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
// Game state
// ---------------------------------------------------------------------------

struct GameState {
    int seat        = 0;
    int num_players = 2;
    int starting_chips = 1000;
    int swap_mults[4] = {5, 15, 25, 50};  // PREFLOP, FLOP, TURN, RIVER

    std::vector<Card> hole_cards;
    std::vector<Card> community;
    std::vector<int>  chips;
    int my_chips = 1000;
    int small_blind = 1;
    int num_active  = 1;   // opponents still in current hand
    int street_idx  = 0;   // 0=pre, 1=flop, 2=turn, 3=river
    int last_swap_idx   = 0;
    int swap_count_phase = 0;  // limit: 1 swap per phase

    void reset_for_hand(int active_opps) {
        hole_cards.clear();
        community.clear();
        num_active = active_opps;
        street_idx = 0;
        swap_count_phase = 0;
    }
};

// ---------------------------------------------------------------------------
// Decision helpers
// ---------------------------------------------------------------------------

static std::string decide_action(GameState& st, int chips, int current_bet,
                                  int your_bet, int min_raise, int pot)
{
    int to_call = current_bet - your_bet;

    // Must go all-in or fold
    if (to_call >= chips) {
        double wp = win_probability(st.hole_cards, st.community, st.num_active);
        return (wp > 0.45) ? "ALLIN" : "FOLD";
    }

    double wp = win_probability(st.hole_cards, st.community, st.num_active);

    if (to_call == 0) {
        if (wp > 0.65) {
            int raise_amt = std::max(min_raise, pot / 2);
            raise_amt = std::min(raise_amt, chips);
            if (raise_amt >= min_raise) {
                return "RAISE " + std::to_string(raise_amt);
            }
        }
        return "CHECK";
    }

    double pot_after = pot + to_call;
    double pot_odds  = (pot_after > 0) ? (to_call / pot_after) : 0.0;

    if (wp > 0.75) {
        int raise_amt = std::max(min_raise, (int)(pot * 0.75));
        raise_amt = std::min(raise_amt, chips);
        if (raise_amt >= min_raise)
            return "RAISE " + std::to_string(raise_amt);
        return "CALL";
    }
    if (wp > pot_odds * 1.1)  return "CALL";
    if (wp > pot_odds * 0.85 && to_call <= (int)(chips * 0.08)) return "CALL";
    return "FOLD";
}

static std::string decide_swap(GameState& st, int cost, int my_chips, int samples = 20) {
    if (cost > my_chips * 0.30) return "STAY";
    if ((int)st.hole_cards.size() < 2) return "STAY";

    double cur = win_probability(st.hole_cards, st.community, st.num_active, 20);

    // Build remaining deck (excluding known cards)
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
        Card other = st.hole_cards[1 - i];
        double total = 0.0;
        for (auto& rep : sample) {
            std::vector<Card> test_hole = {other, rep};
            total += win_probability(test_hole, st.community, st.num_active, 10);
        }
        double avg = total / samples;
        double improvement = avg - cur;
        if (improvement > best_improvement) {
            best_improvement = improvement;
            best_i = i;
        }
    }

    if (best_i >= 0 && best_improvement > 0.05 && cost <= my_chips * 0.15) {
        return "SWAP " + std::to_string(best_i);
    }
    return "STAY";
}

static std::string decide_vote(GameState& st, int my_chips) {
    if (st.community.empty()) return "VOTE YES 0";

    double wp = win_probability(st.hole_cards, st.community, st.num_active, 20);
    double baseline = 1.0 / (st.num_active + 1);
    double confidence = std::abs(wp - baseline);
    int wager = (int)(my_chips * std::min(0.08, confidence * 0.15));
    wager = std::max(0, wager);

    if (wp >= baseline)
        return "VOTE YES " + std::to_string(wager);
    return "VOTE NO " + std::to_string(wager);
}

// ---------------------------------------------------------------------------
// Message loop
// ---------------------------------------------------------------------------

int main() {
    // Flush stdout after every output — the engine reads line-by-line with a 10ms timeout
    std::cout << std::unitbuf;
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    init_deck();
    GameState st;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "GAME_START") {
            // GAME_START <num_players> <your_seat> <starting_chips> <pf_m> <fl_m> <tu_m> <ri_m>
            int np, seat, sc, m0, m1, m2, m3;
            iss >> np >> seat >> sc >> m0 >> m1 >> m2 >> m3;
            st.num_players   = np;
            st.seat          = seat;
            st.starting_chips = sc;
            st.swap_mults[0] = m0;
            st.swap_mults[1] = m1;
            st.swap_mults[2] = m2;
            st.swap_mults[3] = m3;
            st.my_chips      = sc;
            st.chips.assign(np, sc);

        } else if (cmd == "HAND_START") {
            // HAND_START <hand_num> <dealer> <sb_seat> <bb_seat> <sb_amt> <bb_amt>
            int hn, dealer, sb_s, bb_s, sba, bba;
            iss >> hn >> dealer >> sb_s >> bb_s >> sba >> bba;
            st.small_blind = sba;
            int active = 0;
            for (int i = 0; i < (int)st.chips.size(); i++)
                if (i != st.seat && st.chips[i] > 0) active++;
            st.reset_for_hand(active);

        } else if (cmd == "CHIPS") {
            st.chips.clear();
            int c;
            while (iss >> c) st.chips.push_back(c);
            if (st.seat < (int)st.chips.size())
                st.my_chips = st.chips[st.seat];

        } else if (cmd == "DEAL_HOLE") {
            std::string c1, c2;
            iss >> c1 >> c2;
            st.hole_cards = {parse_card(c1), parse_card(c2)};

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

        } else if (cmd == "SWAP_RESULT") {
            std::string c1; iss >> c1;
            if (st.last_swap_idx < (int)st.hole_cards.size())
                st.hole_cards[st.last_swap_idx] = parse_card(c1);

        } else if (cmd == "REDRAW_FLOP") {
            std::string c1, c2, c3;
            iss >> c1 >> c2 >> c3;
            st.community = {parse_card(c1), parse_card(c2), parse_card(c3)};
            st.street_idx = 1;

        } else if (cmd == "REDRAW_TURN") {
            std::string c1; iss >> c1;
            Card turn = parse_card(c1);
            if (st.community.size() >= 4) st.community[3] = turn;
            else st.community.push_back(turn);
            st.street_idx = 2;

        } else if (cmd == "REDRAW_RIVER") {
            std::string c1; iss >> c1;
            Card river = parse_card(c1);
            if (st.community.size() >= 5) st.community[4] = river;
            else st.community.push_back(river);
            st.street_idx = 3;

        } else if (cmd == "ACTION") {
            // ACTION <seat> <action> [<amount>]
            int seat; std::string action;
            iss >> seat >> action;
            if (action == "FOLD" && seat != st.seat)
                st.num_active = std::max(0, st.num_active - 1);

        } else if (cmd == "ELIMINATE") {
            int seat; iss >> seat;
            if (seat < (int)st.chips.size()) st.chips[seat] = 0;

        } else if (cmd == "GAME_OVER") {
            break;

        // ---- Prompts: must respond immediately ----

        } else if (cmd == "ACTION_PROMPT") {
            int chips, cb, yb, mr, pot;
            iss >> chips >> cb >> yb >> mr >> pot;
            st.my_chips = chips;
            std::cout << decide_action(st, chips, cb, yb, mr, pot) << "\n";

        } else if (cmd == "SWAP_PROMPT") {
            int cost, chips;
            iss >> cost >> chips;
            st.my_chips = chips;
            if (st.swap_count_phase >= 1) {
                std::cout << "STAY\n";
            } else {
                std::string resp = decide_swap(st, cost, chips);
                if (resp.rfind("SWAP ", 0) == 0) {
                    st.last_swap_idx = resp[5] - '0';
                    st.swap_count_phase++;
                }
                std::cout << resp << "\n";
            }

        } else if (cmd == "VOTE_PROMPT") {
            int chips; iss >> chips;
            st.my_chips = chips;
            std::cout << decide_vote(st, chips) << "\n";
        }
        // All other messages (WINNER, SHOWDOWN, VOTE_RESULT, SWAP_DONE, etc.) are informational
    }
    return 0;
}
