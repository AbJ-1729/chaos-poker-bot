#!/usr/bin/env python3
"""
Chaos Poker bot using fast hand evaluation with Monte Carlo win estimation.

Strategy:
- ACTION: compare win probability against pot odds; raise strong, call positive EV, fold otherwise
- SWAP:   estimate improvement from random card replacement vs cost
- VOTE:   compare current board strength to random-board baseline; wager proportional to confidence

Performance design: avoid expensive itertools.combinations for 7-card evaluation; use direct
rank/suit analysis instead. Monte Carlo uses 25 simulations only (fast enough for 10ms limit).
"""

import sys
import random
from collections import Counter

# ---------------------------------------------------------------------------
# Card utilities
# ---------------------------------------------------------------------------

RANK_MAP = {'2': 2, '3': 3, '4': 4, '5': 5, '6': 6, '7': 7, '8': 8,
            '9': 9, 'T': 10, 'J': 11, 'Q': 12, 'K': 13, 'A': 14}
SUIT_MAP = {'s': 0, 'h': 1, 'd': 2, 'c': 3}

FULL_DECK = tuple((r, s) for r in range(2, 15) for s in range(4))


def parse_card(s):
    """Parse 'As' -> (14, 0)."""
    return (RANK_MAP[s[0]], SUIT_MAP[s[1]])


# ---------------------------------------------------------------------------
# Fast 7-card hand evaluator (no itertools)
# ---------------------------------------------------------------------------

def _best_straight(rank_list):
    """Return the highest straight top card from a sorted-descending unique rank list, or 0."""
    if not rank_list:
        return 0
    # Add ace-low
    if 14 in rank_list:
        rank_list = list(rank_list) + [1]
    rank_set = set(rank_list)
    for top in sorted(rank_list, reverse=True):
        if all(top - i in rank_set for i in range(5)):
            return top
    return 0


def evaluate_seven(cards):
    """
    Evaluate best 5-card poker hand from exactly 7 cards.
    Returns (rank_int, kickers) where rank_int:
      8=straight flush, 7=quads, 6=full house, 5=flush,
      4=straight, 3=trips, 2=two pair, 1=pair, 0=high card
    Higher tuple = stronger hand.
    """
    ranks = [c[0] for c in cards]
    suits = [c[1] for c in cards]

    suit_cnt = Counter(suits)
    rank_cnt = Counter(ranks)

    # Check for flush suit (>= 5 cards)
    flush_suit = next((s for s, n in suit_cnt.items() if n >= 5), None)

    rank_sorted_desc = sorted(set(ranks), reverse=True)

    str_high = _best_straight(rank_sorted_desc)

    if flush_suit:
        flush_ranks = sorted([r for r, s in zip(ranks, suits) if s == flush_suit], reverse=True)
        flush_rank_set = sorted(set(flush_ranks), reverse=True)
        sf_high = _best_straight(flush_rank_set)
        if sf_high:
            return (8, [sf_high])
        return (5, flush_ranks[:5])

    # Sort by (frequency desc, rank desc)
    freq = sorted(rank_cnt.items(), key=lambda x: (x[1], x[0]), reverse=True)
    g = [cnt for _, cnt in freq]
    gr = [r for r, _ in freq]

    if g[0] == 4:
        kicker = max(r for r in ranks if r != gr[0])
        return (7, [gr[0], kicker])

    if g[0] == 3 and g[1] >= 2:
        return (6, [gr[0], gr[1]])

    if str_high:
        return (4, [str_high])

    if g[0] == 3:
        kickers = sorted([r for r in ranks if r != gr[0]], reverse=True)[:2]
        return (3, [gr[0]] + kickers)

    if g[0] == 2 and g[1] == 2:
        p1, p2 = sorted([gr[0], gr[1]], reverse=True)
        kicker = max(r for r in ranks if r != p1 and r != p2)
        return (2, [p1, p2, kicker])

    if g[0] == 2:
        kickers = sorted([r for r in ranks if r != gr[0]], reverse=True)[:3]
        return (1, [gr[0]] + kickers)

    return (0, sorted(ranks, reverse=True)[:5])


def evaluate_five(cards):
    """Evaluate exactly 5 cards. Returns same format as evaluate_seven."""
    ranks = [c[0] for c in cards]
    suits = [c[1] for c in cards]

    flush = len(set(suits)) == 1
    r_sorted = sorted(ranks, reverse=True)
    r_set = sorted(set(ranks), reverse=True)

    str_high = _best_straight(r_set)

    if str_high and flush:
        return (8, [str_high])

    rank_cnt = Counter(ranks)
    freq = sorted(rank_cnt.items(), key=lambda x: (x[1], x[0]), reverse=True)
    g = [cnt for _, cnt in freq]
    gr = [r for r, _ in freq]

    if g[0] == 4:
        return (7, [gr[0], gr[1]])
    if g[0] == 3 and g[1] == 2:
        return (6, [gr[0], gr[1]])
    if flush:
        return (5, r_sorted)
    if str_high:
        return (4, [str_high])
    if g[0] == 3:
        kickers = sorted([r for r in ranks if r != gr[0]], reverse=True)
        return (3, [gr[0]] + kickers)
    if g[0] == 2 and g[1] == 2:
        p1, p2 = sorted([gr[0], gr[1]], reverse=True)
        kicker = max(r for r in ranks if r != p1 and r != p2)
        return (2, [p1, p2, kicker])
    if g[0] == 2:
        kickers = sorted([r for r in ranks if r != gr[0]], reverse=True)
        return (1, [gr[0]] + kickers)
    return (0, r_sorted)


def evaluate_best_hand(cards):
    """Best 5-card hand from 5-7 cards."""
    n = len(cards)
    if n == 7:
        return evaluate_seven(cards)
    if n == 5:
        return evaluate_five(cards)
    # 6 cards: try all C(6,5)=6 combinations
    best = None
    for i in range(n):
        score = evaluate_five([cards[j] for j in range(n) if j != i])
        if best is None or score > best:
            best = score
    return best


# ---------------------------------------------------------------------------
# Preflop strength table (heads-up; adjusted for multi-player below)
# ---------------------------------------------------------------------------

def preflop_strength_hu(c1, c2):
    """
    Approximate heads-up win probability for a starting hand (preflop, no community cards).
    Based on standard equity tables simplified to a formula.
    """
    r1, s1 = c1
    r2, s2 = c2
    r_hi, r_lo = max(r1, r2), min(r1, r2)
    suited = (s1 == s2)

    if r1 == r2:
        # Pocket pairs: AA≈85%, 22≈52%, linear interpolation
        return 0.52 + (r_hi - 2) * (0.85 - 0.52) / 12

    # Non-pair hands
    base = 0.37 + (r_hi - 2) * 0.19 / 12 + (r_lo - 2) * 0.07 / 12
    suited_bonus = 0.030 if suited else 0
    gap = r_hi - r_lo - 1
    conn_bonus = max(0.0, (4 - gap) * 0.004) if gap <= 4 else 0
    raw = base + suited_bonus + conn_bonus

    # Penalise weak high-card hands (low card dominant in reality vs formula)
    if r_hi <= 9:
        raw *= 0.82
    elif r_hi <= 11:
        raw *= 0.93

    return min(0.70, raw)


# ---------------------------------------------------------------------------
# Win probability (Monte Carlo with fast evaluator)
# ---------------------------------------------------------------------------

def win_probability(hole_cards, community_cards, num_opponents, simulations=50):
    """
    Estimate probability of winning using Monte Carlo simulation.
    Optimised for speed: uses evaluate_seven, only 25 simulations by default.
    """
    if num_opponents <= 0:
        return 1.0

    # Preflop shortcut: use lookup table (no Monte Carlo needed)
    if not community_cards and len(hole_cards) == 2:
        base = preflop_strength_hu(hole_cards[0], hole_cards[1])
        # Scale down for multiple opponents
        strength = base
        for _ in range(num_opponents - 1):
            strength *= 0.82
        return max(0.01, min(0.99, strength))

    known = set(hole_cards) | set(community_cards)
    remaining = [c for c in FULL_DECK if c not in known]
    cards_needed = 5 - len(community_cards)

    wins = 0
    ties = 0
    rand_shuffle = random.shuffle

    for _ in range(simulations):
        rand_shuffle(remaining)
        idx = cards_needed
        sim_comm = community_cards + remaining[:idx]

        my_cards = hole_cards + sim_comm
        my_score = evaluate_best_hand(my_cards) if len(my_cards) != 7 else evaluate_seven(my_cards)

        best_opp = None
        for _ in range(num_opponents):
            opp_hole = remaining[idx: idx + 2]
            idx += 2
            if len(opp_hole) < 2:
                break
            opp_cards = opp_hole + sim_comm
            opp_score = (evaluate_best_hand(opp_cards)
                         if len(opp_cards) != 7 else evaluate_seven(opp_cards))
            if best_opp is None or opp_score > best_opp:
                best_opp = opp_score

        if best_opp is None:
            wins += 1
        elif my_score > best_opp:
            wins += 1
        elif my_score == best_opp:
            ties += 1

    return (wins + 0.5 * ties) / simulations


# ---------------------------------------------------------------------------
# Game state
# ---------------------------------------------------------------------------

class GameState:
    def __init__(self):
        # Set once at GAME_START
        self.seat = 0
        self.num_players = 2
        self.starting_chips = 1000
        self.swap_mults = {'PREFLOP': 5, 'FLOP': 15, 'TURN': 25, 'RIVER': 50}

        # Per-hand state
        self.hole_cards = []
        self.community = []
        self.my_chips = 1000
        self.chips = []
        self.small_blind = 1
        self.num_active = 1    # opponents still live in current hand
        self.street = 'PREFLOP'
        self.hand_num = 0
        self._last_swap_index = 0
        self._swap_count_this_phase = 0  # limit re-swaps within one phase

    def reset_for_hand(self, num_active_opponents):
        self.hole_cards = []
        self.community = []
        self.street = 'PREFLOP'
        self.num_active = num_active_opponents
        self._swap_count_this_phase = 0


# ---------------------------------------------------------------------------
# Decision functions
# ---------------------------------------------------------------------------

def decide_action(state, chips, current_bet, your_bet, min_raise, pot):
    """Return action string for ACTION_PROMPT."""
    to_call = current_bet - your_bet

    # Can't call more than we have
    if to_call >= chips:
        win_prob = win_probability(state.hole_cards, state.community, state.num_active)
        return 'ALLIN' if win_prob > 0.45 else 'FOLD'

    win_prob = win_probability(state.hole_cards, state.community, state.num_active)

    if to_call == 0:
        if win_prob > 0.65:
            raise_amt = max(min_raise, pot // 2)
            raise_amt = min(raise_amt, chips)
            if raise_amt >= min_raise:
                return f'RAISE {raise_amt}'
        return 'CHECK'

    pot_after_call = pot + to_call
    pot_odds = to_call / pot_after_call if pot_after_call > 0 else 0

    if win_prob > 0.75:
        raise_amt = max(min_raise, int(pot * 0.75))
        raise_amt = min(raise_amt, chips)
        if raise_amt >= min_raise:
            return f'RAISE {raise_amt}'
        return 'CALL'
    elif win_prob > pot_odds * 1.1:
        return 'CALL'
    elif win_prob > pot_odds * 0.85 and to_call <= chips * 0.08:
        return 'CALL'
    else:
        return 'FOLD'


def decide_swap(state, cost, my_chips):
    """Return swap decision string for SWAP_PROMPT."""
    if cost > my_chips * 0.30:
        return 'STAY'
    if len(state.hole_cards) < 2:
        return 'STAY'

    current_strength = win_probability(state.hole_cards, state.community,
                                       state.num_active, simulations=15)

    known = set(state.hole_cards) | set(state.community)
    remaining = [c for c in FULL_DECK if c not in known]

    improvements = []
    sample_size = min(15, len(remaining))
    if sample_size == 0:
        return 'STAY'

    sample = random.sample(remaining, sample_size)
    for i in range(2):
        other = [state.hole_cards[1 - i]]
        total = sum(
            win_probability(other + [rep], state.community,
                            state.num_active, simulations=10)
            for rep in sample
        )
        improvements.append(total / sample_size - current_strength)

    best_i = 0 if improvements[0] >= improvements[1] else 1

    if improvements[best_i] > 0.05 and cost <= my_chips * 0.15:
        return f'SWAP {best_i}'
    return 'STAY'


def decide_vote(state, my_chips):
    """Return vote decision string for VOTE_PROMPT."""
    if not state.community:
        return 'VOTE YES 0'

    current_strength = win_probability(state.hole_cards, state.community,
                                       state.num_active, simulations=20)

    baseline = 1.0 / (state.num_active + 1)
    confidence = abs(current_strength - baseline)
    wager = int(my_chips * min(0.08, confidence * 0.15))
    wager = max(0, wager)

    if current_strength >= baseline:
        return f'VOTE YES {wager}'
    return f'VOTE NO {wager}'


# ---------------------------------------------------------------------------
# Main message loop
# ---------------------------------------------------------------------------

def main():
    state = GameState()

    for raw_line in sys.stdin:
        line = raw_line.strip()
        if not line:
            continue
        parts = line.split()
        if not parts:
            continue
        msg = parts[0]

        if msg == 'GAME_START':
            # GAME_START <num_players> <your_seat> <starting_chips> <pf_m> <fl_m> <tu_m> <ri_m>
            state.num_players = int(parts[1])
            state.seat = int(parts[2])
            state.starting_chips = int(parts[3])
            state.swap_mults['PREFLOP'] = int(parts[4])
            state.swap_mults['FLOP'] = int(parts[5])
            state.swap_mults['TURN'] = int(parts[6])
            state.swap_mults['RIVER'] = int(parts[7])
            state.my_chips = state.starting_chips
            state.chips = [state.starting_chips] * state.num_players

        elif msg == 'HAND_START':
            # HAND_START <hand_num> <dealer_seat> <sb_seat> <bb_seat> <sb_amount> <bb_amount>
            state.hand_num = int(parts[1])
            state.small_blind = int(parts[5])
            num_opp = sum(1 for i, c in enumerate(state.chips)
                          if i != state.seat and c > 0)
            state.reset_for_hand(num_opp)

        elif msg == 'CHIPS':
            # CHIPS <seat0_chips> <seat1_chips> ...
            state.chips = [int(x) for x in parts[1:]]
            if state.seat < len(state.chips):
                state.my_chips = state.chips[state.seat]

        elif msg == 'DEAL_HOLE':
            state.hole_cards = [parse_card(parts[1]), parse_card(parts[2])]

        elif msg == 'DEAL_FLOP':
            state.community = [parse_card(parts[1]), parse_card(parts[2]), parse_card(parts[3])]
            state.street = 'FLOP'
            state._swap_count_this_phase = 0

        elif msg == 'DEAL_TURN':
            state.community.append(parse_card(parts[1]))
            state.street = 'TURN'
            state._swap_count_this_phase = 0

        elif msg == 'DEAL_RIVER':
            state.community.append(parse_card(parts[1]))
            state.street = 'RIVER'
            state._swap_count_this_phase = 0

        elif msg == 'SWAP_RESULT':
            # Engine tells us the new card we received after swapping
            new_card = parse_card(parts[1])
            idx = state._last_swap_index
            if idx < len(state.hole_cards):
                state.hole_cards[idx] = new_card

        elif msg in ('REDRAW_FLOP', 'REDRAW_TURN', 'REDRAW_RIVER'):
            state.community = [parse_card(c) for c in parts[1:]]

        elif msg == 'ACTION':
            # ACTION <seat> <action> [<amount>]
            if len(parts) >= 3:
                seat = int(parts[1])
                action = parts[2]
                if action == 'FOLD' and seat != state.seat:
                    state.num_active = max(0, state.num_active - 1)

        elif msg == 'ELIMINATE':
            seat = int(parts[1])
            if seat < len(state.chips):
                state.chips[seat] = 0

        elif msg in ('WINNER', 'SHOWDOWN', 'VOTE_RESULT', 'SWAP_DONE'):
            pass

        elif msg == 'GAME_OVER':
            break

        # ---- Prompts: require a response ----
        elif msg == 'ACTION_PROMPT':
            # ACTION_PROMPT <chips> <current_bet> <your_bet> <min_raise> <pot>
            chips = int(parts[1])
            current_bet = int(parts[2])
            your_bet = int(parts[3])
            min_raise = int(parts[4])
            pot = int(parts[5])
            state.my_chips = chips
            response = decide_action(state, chips, current_bet, your_bet, min_raise, pot)
            print(response, flush=True)

        elif msg == 'SWAP_PROMPT':
            # SWAP_PROMPT <cost_per_card> <your_chips>
            cost = int(parts[1])
            my_chips = int(parts[2])
            state.my_chips = my_chips
            # Limit to one swap per phase to avoid bleeding chips on re-swaps
            if state._swap_count_this_phase >= 1:
                print('STAY', flush=True)
            else:
                response = decide_swap(state, cost, my_chips)
                if response.startswith('SWAP '):
                    state._last_swap_index = int(response.split()[1])
                    state._swap_count_this_phase += 1
                print(response, flush=True)

        elif msg == 'VOTE_PROMPT':
            # VOTE_PROMPT <your_chips>
            my_chips = int(parts[1])
            state.my_chips = my_chips
            response = decide_vote(state, my_chips)
            print(response, flush=True)


def _warmup():
    """
    Force Python to JIT-warm all hot code paths at startup.
    Python startup + first-time module imports take ~30ms. We burn that time here,
    before the engine starts the 10ms clock on our first real prompt.
    This function runs on import and should complete in < 50ms total.
    """
    dummy_cards = [(14, 0), (13, 1), (12, 2), (11, 3), (10, 0), (9, 1), (8, 2)]
    evaluate_seven(dummy_cards)
    evaluate_best_hand(dummy_cards[:5])
    win_probability([(14, 0), (13, 1)], [(12, 2), (11, 3), (10, 0)], 1, simulations=10)
    preflop_strength_hu((14, 0), (13, 1))


_warmup()


if __name__ == '__main__':
    try:
        main()
    except (BrokenPipeError, IOError):
        pass
