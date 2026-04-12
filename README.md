# Chaos Poker Bot

**Abhijai Chugh**

## Launch command

```bash
./bots/my_bot
```

## Build

Requires `g++` with `C++17` support. This bot is intended to run on a Linux environment (or WSL on Windows).

```bash
make
```

This builds the engine (`chaos_poker`) and all standard C++ bots under `bots/`, including `bots/my_bot`.

## Run

To quickly test the bot against the provided sample bots across 100 hands:

```bash
./test_run.sh
```

Or to test a single game:
```bash
./chaos_poker --history 1000 5 15 25 50 ./bots/my_bot ./bots/example_bot ./bots/random_bot
```

---

## Strategy

The bot is written in **C++** (`bots/my_bot.cpp`) to easily support heavy mathematical computation (Monte Carlo simulations) while remaining comfortably within the strict 10ms execution deadline. The typical worst-case response time sits at **~1ms-3ms**.

The overarching philosophy of the bot is **"Maximal Exploitative Deviation."** By default, it plays a high-value, un-exploitable base game, but aggressively shifts to exploit specific opponents as it gathers data on their tendencies.

### 1. Opponent Modeling & Bluff Catching
The bot tracks per-seat behaviors over multiple hands by watching `ACTION`, `SHOWDOWN`, and `WINNER` broadcasts:
- **Fold Rate**: Gates the bot's own bluffing frequency. If an opponent never folds (a "calling station"), our bluffing frequency is literally clamped to `0%`. We play pure value poker against them. 
- **Aggression vs Showdown Strength**: By combining a player's betting propensity (Aggression Factor) with the actual strength of the hands they show down, the bot profiles them. If they bet heavily but show weak cards, they are tagged as "bluffers." The bot will artificially inflate its own calling range and catch their bluffs with lighter holdings.
- **Vote Tendencies**: Tracks how often opponents vote YES vs NO and deduce their wagers.

*Note: All stats require a minimum sample size (20+ actions) before the bot acts on them, avoiding small-sample noise in early hands.*

### 2. Evidence-Based Bluffing
All bluffs are gated behind the `bluff_viable()` check, which tests if the remaining opponents have an average fold rate of > 40%. Bluffing frequencies then directly scale with the opponent's exploitability.
- **C-Bets:** Fired on dry boards (e.g., `K-7-2`) when we raised pre-flop.
- **Double Barrels:** Fired on the turn when a C-bet was called.
- **Semi-Bluffs:** Straight and Flush draws are bet aggressively. "Combo Draws" (Flush + Open Ended Straight = 15 outs) are always bet as they hold >55% equity and are mathematically value bets.
- **Positional Steals:** Pure air bet when acting last and everyone checks to us.

### 3. Smart Swap (Hand Preservation)
The swap logic (`SWAP_PROMPT`) seeks to replace awful cards but explicitly protects structurally sound ones:
- **Pre-Flop Protection:** Never swaps Pocket Pairs, Suited Connectors, Ace-X suited, or Premium Broadway combinations (`KQ`, `JT`), regardless of current win probabilities.
- **Post-Flop Draw Protection:** Never swaps a card that's contributing to an active flush or straight draw.
- **Simulation**: Uses a fast 15-sample Monte Carlo with 8 inner simulations per sample to see if a random card replacement definitively increases expected equity compared to staying. Limit of 1 swap per phase to prevent chip drain.

### 4. Vote Disruption & Board Torpedoing
The bot views the chaos vote not just as a way to fix its own hand, but as a weapon to sabotage opponents:
- **Strong Protection**: If the bot is holding a monster (`>80% equity`), it leverages the fact that its wager goes into a pot it's likely to win. It bets heavily (up to 35% of its stack) YES to prevent opponents from redrawing the board.
- **Board Torpedoing**: If the bot's equity is weak (`<35%`) but an opponent goes `ALLIN` or raises heavily, the bot infers the opponent *needs* this current board. It will vote `NO` with a heavy wager to deliberately torpedo the opponent's strong hand.
- **Chip Position**: A short-stacked bot will slightly bias votes towards `NO` (increasing variance), while a chip leader biases towards `YES` (forcing stability).

### 5. High-Fidelity Monte Carlo
The core engine runs 400–1000 simulations per `ACTION_PROMPT` depending on the street:
- **Preflop**: Fast table-based analytical approximation.
- **Flop**: 400 sims.
- **Turn**: 600 sims.
- **River**: 1000 sims.
The C++ implementation ensures these complete almost instantaneously, leaving a wide safety margin. Random jitter (±10%) is applied to all calculated bet sizes to obscure the bot's algorithms from opponents.
