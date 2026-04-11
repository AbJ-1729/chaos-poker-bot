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

**Betting (ACTION_PROMPT):** Estimates win probability via Monte Carlo simulation against random opponent hands, with simulation count scaled by street (30 on flop, 40 on turn, 50 on river — more cards are known on later streets so each run is cheaper). At preflop an analytical approximation based on standard equity tables is used instead. Raises to ~75% of pot when win probability exceeds 75%, calls when EV is positive against pot odds, and folds otherwise.

**Swap (SWAP_PROMPT):** Samples 20 random replacement cards for each hole card and estimates the average improvement in win probability. Swaps the card with the highest expected gain if it exceeds 5 percentage points and the cost is under 15% of stack. Limits to one swap per street phase to avoid chip bleed from repeated marginal swaps.

**Vote (VOTE_PROMPT):** Rather than comparing against a uniform baseline, estimates the expected win probability if the current street's cards were redrawn (sampled over 12 random replacements). Votes YES to keep the board if the current equity exceeds the redraw expectation, NO otherwise. Wagers proportional to the edge, capped at 10% of stack.

**Tradeoffs:** Monte Carlo accuracy is deliberately bounded to stay well inside the 10 ms response limit. The swap logic is conservative (one swap per phase, >5% improvement threshold) to avoid bleeding chips on marginal edges. A more aggressive approach could extract more EV from strong hands at the cost of larger variance.
