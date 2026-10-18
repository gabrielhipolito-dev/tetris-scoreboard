# Tetris Battle Scoreboard

A Windows desktop app that listens to an Arduino Tetris game over a USB serial cable. It shows the match result (who won, scores, levels), keeps a leaderboard, player records and match history, and saves everything to a CSV file.

Built with Python and PySide6 (Qt). It does not use a web browser, Node.js or Figma. Figma was only used to design how it looks.

---

## 1. How it works

```
Arduino game --USB serial (115200 baud)--> app.py --> scoreboard.csv
                                              |
                                              +--> desktop window (Match, Leaderboard, Records, History)
```

1. When a player finishes typing their name on the Arduino, the Arduino sends `PING`.
2. `app.py` replies `PONG`. If no reply arrives within 5 seconds, the Arduino shows "OFFLINE MODE" and the score is not sent.
3. After the `PONG`, the Arduino sends one line of JSON with that player's result.
4. In a 2-player game, `app.py` waits until both players have sent a result with the same `game` number, decides the winner, saves one row to `scoreboard.csv` and updates the window.
5. In a 1-player game, the result is saved immediately as a solo run.

### Data the Arduino sends

```json
{"event":"SCORE_SUBMIT","player":1,"name":"MAYA","score":12480,"level":6,"game":7,"mode":"2P","won":1}
```

| Field | Meaning |
|---|---|
| `event` | Always `SCORE_SUBMIT`. Any other line is ignored. |
| `player` | `1` or `2`. |
| `name` | Name typed by the player (up to 10 characters). |
| `score` | Final score. |
| `level` | Level reached (1 to 10). |
| `game` | Game number. Both players in the same 2P match share it. |
| `mode` | `1P` or `2P`. |
| `won` | `1` if the game reported this player as the winner, otherwise `0`. In `1P` mode it is ignored. |

The date and time are added by the PC when the row is saved.

### How the winner is decided (2P)

1. If exactly one player has `won = 1`, that player wins.
2. Otherwise the higher score wins.
3. If the scores are equal, it is a tie.

---

## 2. Folder layout

```
tetris-scoreboard/
  .venv/                    Python virtual environment (created by you)
  app.py                    The whole application
  fonts/
    PressStart2P-Regular.ttf   Pixel font used for the logo, names and scores
  requirements.txt          List of installed Python packages
  scoreboard.csv            Created automatically when the first score is saved
  README.md                 This file
```

Keep the `fonts` folder next to `app.py`. Without it, the app still works but uses a normal font for names and scores.

---

## 3. Requirements

| Item | Why |
|---|---|
| Windows 10 or 11 | The exe is built for Windows. |
| Python 3.11 or newer (from python.org) | Runs the app. Tick "Add Python to PATH" during setup. |
| Arduino IDE | Uploads the sketch to the board. |
| A USB cable to the Arduino | Carries the serial data. |

You do not need Node.js, npm or pnpm for this version.

### 3.1 Clone the project

Install [Git](https://git-scm.com/downloads) if it is not already installed, then open a terminal and run:

```
git clone https://github.com/gabrielhipolito-dev/tetris-scoreboard.git
cd tetris-scoreboard
```

This downloads the project into a new `tetris-scoreboard` folder and moves your terminal into that folder. You can then continue with the setup steps below.

---

## 4. One-time setup, command by command

All commands below are typed in a terminal opened inside the `tetris-scoreboard` folder. In VS Code: **Terminal > New Terminal**.

### 4.1 Go to the project folder

```
cd tetris-scoreboard
```

`cd` means "change directory". It moves the terminal into the project folder, so every following command runs there. Use the full path if the folder is somewhere else, for example `cd ~/OneDrive/desktop/tetris-scoreboard`.

### 4.2 Create the virtual environment

```
python -m venv .venv
```

- `python` starts Python.
- `-m venv` tells Python to run its built-in `venv` module.
- `.venv` is the name of the folder to create.

A virtual environment is a private copy of Python for this project. Packages installed into it do not affect the rest of your computer, and PyInstaller bundles only what the project needs. You do this once per project. Do not move or copy the `.venv` folder. If you move the project, delete `.venv` and create it again.

### 4.3 Activate the virtual environment

The command depends on the terminal you use.

| Terminal | Command |
|---|---|
| Git Bash (prompt shows `MINGW64`) | `source .venv/Scripts/activate` |
| Command Prompt (cmd) | `.venv\Scripts\activate` |
| PowerShell | `.venv\Scripts\Activate.ps1` |

- `source` (Git Bash) runs the activation script inside the current terminal, instead of in a separate process, so the change sticks.
- `.venv/Scripts/activate` is the path to the activation script. Git Bash needs forward slashes `/`. Typing backslashes `\` there fails with `command not found`, because Git Bash treats `\` as an escape character.

When it works, the prompt starts with `(.venv)`. **Activate again every time you open a new terminal.**

If PowerShell says "running scripts is disabled", run this once and try again:

```
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
```

This allows locally created scripts to run for your user account only.

### 4.4 Install the Python packages

```
pip install PySide6 pyserial pyinstaller
```

- `pip` is Python's package installer.
- `install` downloads and installs packages.
- `PySide6` is the Qt library that draws the window, cards, tables and animations.
- `pyserial` reads and writes the USB serial port to talk to the Arduino.
- `pyinstaller` packs the app into a single `.exe` file.

Because the venv is active, they are installed only into `.venv`.

### 4.5 Save the package list

```
pip freeze > requirements.txt
```

- `pip freeze` prints every installed package with its exact version.
- `>` sends that output into a file instead of showing it on screen.
- `requirements.txt` is the file name.

Later, on a new computer, `pip install -r requirements.txt` installs the same packages. `-r` means "read the list from this file".

---

## 5. Arduino setup

The sketch needs a few additions so it sends `game`, `mode` and `won`.

1. Add a global counter above `class TetrisGame`:
   ```cpp
   uint16_t gameCounter = 0;
   ```
2. Add three variables inside `TetrisGame` (private section):
   ```cpp
   bool lastWon = false;
   bool wasTwoPlayer = false;
   uint16_t gameId = 0;
   ```
3. At the start of `start1PGame()`:
   ```cpp
   gameCounter++;
   gameId = gameCounter;
   ```
4. At the start of `start2PGame()`:
   ```cpp
   gameId = gameCounter;
   ```
5. In `loop()`, in the 2-player synchronized start, add one line before the two `start2PGame()` calls:
   ```cpp
   gameCounter++;
   ```
6. In `triggerGameOver`, right before `gameState = STATE_GAMEOVER;` (and after the `diedIn2PFlag` line):
   ```cpp
   wasTwoPlayer = (gameState == STATE_PLAYING_2P);
   lastWon = won;
   ```
7. In the `SCORE_SUBMIT` block, right before `Serial.println(F("}"));`:
   ```cpp
   Serial.print(F(",\"game\":")); Serial.print(gameId);
   Serial.print(F(",\"mode\":\"")); Serial.print(wasTwoPlayer ? F("2P") : F("1P")); Serial.print('"');
   Serial.print(F(",\"won\":")); Serial.print(lastWon ? 1 : 0);
   ```

Upload the sketch with the Arduino IDE. To check, open the Serial Monitor at 115200 baud and finish a game. You should see the JSON line (the PING will wait for a PONG, so the line appears after the 5 second timeout only if the app is not running, which is fine for this check). **Then close the Serial Monitor.**

> Only one program can use a COM port at a time. If the Arduino IDE Serial Monitor is open, the app cannot connect.

---

## 6. Running the app

### 6.1 Demo mode (no Arduino needed)

```
python app.py --demo
```

- `python app.py` runs the program.
- `--demo` is an option (flag). Instead of reading the serial port, the app invents a fake game every few seconds so you can check the window.

Demo games are saved to `scoreboard.csv` like real ones. **Delete the file afterward**, or the fake scores will appear on your real leaderboard:

```
rm scoreboard.csv
```

`rm` removes a file. In Command Prompt use `del scoreboard.csv`.

### 6.2 Real mode

1. Upload the Arduino sketch.
2. Close the Arduino Serial Monitor.
3. Leave the Arduino plugged in by USB.
4. Run:
   ```
   python app.py
   ```

The status in the top-right corner shows "Arduino connected" (green dot) or "Arduino disconnected" (red dot). The app finds the port by itself and reconnects if the cable is unplugged.

To quit, close the window.

---

## 7. What each screen shows

| Tab | Content |
|---|---|
| **Match** | Waiting screen with two empty "Not joined" cards, then the result: names, scores, levels, WINNER / LOSER / TIE labels and a commentary message. Solo runs show one card. If only one player has entered a name, the other card says "Entering name...". Buttons: **Next match** (back to the waiting screen) and **Roast mode** (turns the commentary on or off). |
| **Leaderboard** | The ten highest scores in `scoreboard.csv`. Top 3 ranks are gold, silver and bronze. |
| **Records** | One card per player name: wins, losses, ties, best score and win rate. Sorted by wins. Names are matched in capital letters, so `maya` and `MAYA` count as the same player. |
| **History** | The most recent 50 matches, newest first, with both scores and the winner. |

Colors have fixed meanings: cyan is Player 1, red is Player 2, gold is the winner and rank 1, green is win rate.

---

## 8. The `scoreboard.csv` file

One row per finished game. It is created in the same folder as `app.py` (or next to the exe).

| Column | Meaning |
|---|---|
| `time` | Date and time on the PC when the game was saved. |
| `game` | Game number from the Arduino. |
| `mode` | `1P` or `2P`. |
| `p1_name`, `p1_score`, `p1_level` | Player 1's result. Empty if Player 1 did not play. |
| `p2_name`, `p2_score`, `p2_level` | Player 2's result. Empty if Player 2 did not play. |
| `winner` | `1`, `2`, `tie` or `solo`. |

You can open it in Excel. To reset all scores, close the app and delete the file.

The game number restarts from 1 each time the Arduino is reset. This does not affect the data, because every row also stores the time.

---

## 9. Making the exe, command by command

Do this only after `python app.py --demo` and a real-Arduino test both work.

```
MSYS_NO_PATHCONV=1 pyinstaller --onefile --windowed --add-data "fonts;fonts" app.py
```

| Part | What it does |
|---|---|
| `MSYS_NO_PATHCONV=1` | Git Bash only. Stops Git Bash from rewriting the `fonts;fonts` text as if it were a Unix path. Leave it out in Command Prompt. |
| `pyinstaller` | The tool that packs Python and your code into an exe. |
| `--onefile` | Produce a single `.exe` instead of a folder full of files. |
| `--windowed` | No black console window behind the app. |
| `--add-data "fonts;fonts"` | Copy the `fonts` folder into the exe. Format is `source;destination`. On Windows the separator is `;`. |
| `app.py` | The script to pack. |

It takes a minute or two. PyInstaller creates these:

| Path | What it is |
|---|---|
| `dist/app.exe` | **Your finished app.** |
| `build/` | Temporary working files. Safe to delete. |
| `app.spec` | A saved build recipe. Safe to delete or keep. |

### Using the exe

- Double-click `dist/app.exe`.
- `scoreboard.csv` is saved in the same folder as the exe.
- You can copy the exe to another Windows PC. Python does not need to be installed there.
- The first start can take a few seconds because the single file unpacks itself.

If you change `app.py`, run the PyInstaller command again to rebuild.

---

## 10. Troubleshooting

| Problem | Fix |
|---|---|
| `.venvScriptsactivate: command not found` | You are in Git Bash. Use `source .venv/Scripts/activate` with forward slashes. |
| `python` or `pip` not recognized | Python is not on PATH. Reinstall Python and tick "Add Python to PATH", then open a new terminal. |
| No `(.venv)` in the prompt | Activate again (section 4.3). |
| Status stays "Arduino disconnected" | Check the USB cable, close the Arduino Serial Monitor, and check Device Manager for a COM port. |
| Arduino shows "OFFLINE MODE" | The app was not running, or not connected, when the player submitted. Start the app first. Replies must arrive within 5 seconds. |
| Score never appears for a 2P game | Both players must submit. The first card shows "Entering name..." for the other player until then. |
| Names or scores use a plain font | The `fonts` folder is missing or not next to `app.py`. For the exe, rebuild with `--add-data`. |
| Fake scores on the leaderboard | You ran `--demo`. Close the app and delete `scoreboard.csv`. |
| The exe opens and closes at once | Rebuild without `--windowed`, run it from a terminal, and read the error text. |
| Windows or antivirus warns about the exe | Common with PyInstaller exes. Choose "More info" then "Run anyway", or run with `python app.py`. |
| Very slow `pip install`, "permission denied" or "file in use" errors | The folder is inside OneDrive. Move the project to something like `C:\dev\tetris-scoreboard`, then delete `.venv` and create it again. |

---

## 11. Quick reference

```
# every time you open a new terminal
cd tetris-scoreboard
source .venv/Scripts/activate        # Git Bash

# test with fake games
python app.py --demo
rm scoreboard.csv

# real use
python app.py

# build the exe
MSYS_NO_PATHCONV=1 pyinstaller --onefile --windowed --add-data "fonts;fonts" app.py
```