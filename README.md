# Chaos Poker Bot

**Abhijai Chugh**

## Launch command

```
./bots/my_bot
```

## Build

Requires g++ with C++17 support.

```bash
make
```

This builds the engine (`chaos_poker`) and all bots under `bots/`, including `bots/my_bot`.

## Run

To test the bot against the provided sample bots:

```bash
./chaos_poker --history 1000 5 15 25 50 ./bots/my_bot ./bots/example_bot ./bots/random_bot
```

## Strategy

The bot is implemented in C++ (`bots/my_bot.cpp`) to guarantee sub-millisecond startup and response times, safely within the 10 ms per-prompt limit.

**Betting (ACTION_PROMPT):** Estimates win probability via Monte Carlo simulation (50 runs) against random opponent hands. At preflop, an analytical approximation (based on standard equity tables) replaces Monte Carlo to save time. Raises to ~75% of pot when win probability is high (>75%), calls when expected value is positive against pot odds, and folds otherwise.

**Swap (SWAP_PROMPT):** Samples 20 random replacement cards for each hole card and estimates the average improvement in win probability. Swaps the card with the highest expected gain if it exceeds 5 percentage points and the cost is under 15% of stack. Limits to one swap per street phase to avoid chip bleed.

**Vote (VOTE_PROMPT):** Votes YES if current win probability exceeds the fair-share baseline (1 / players remaining), NO otherwise. Wagers proportional to confidence, capped at 8% of stack.

**Tradeoffs:** Monte Carlo with 50 simulations gives reasonable accuracy but deliberately underestimates variance. The swap and vote logic is intentionally conservative (low wagers, one swap per phase) to avoid losing chips on marginal edges. A more aggressive approach could extract more EV from strong hands at the cost of larger swings.
