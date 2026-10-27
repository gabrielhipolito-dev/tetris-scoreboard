"""Tetris Battle scoreboard: a desktop window that reads the Arduino over serial.

Run: python app.py     (real Arduino)
    python app.py --demo  (fake games, no Arduino)
"""
import argparse
import csv
import json
import os
import random
import sys
import threading
import time
from datetime import datetime

import serial
import serial.tools.list_ports
from PySide6.QtCore import QObject, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QFont, QFontDatabase
from PySide6.QtWidgets import (
  QAbstractItemView, QApplication, QCheckBox, QFrame, QGraphicsDropShadowEffect,
  QGridLayout, QHBoxLayout, QLabel, QMainWindow, QProgressBar, QPushButton,
  QStackedWidget, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget,
)

# ---------- paths (work both as script and as PyInstaller exe) ----------
BASE = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
      else os.path.dirname(os.path.abspath(__file__)))
CSV_FILE = os.path.join(DATA_DIR, "scoreboard.csv")
FONT_FILE = os.path.join(BASE, "fonts", "PressStart2P-Regular.ttf")
BAUD = 115200

FIELDS = ["time", "game", "mode", "p1_name", "p1_score", "p1_level",
     "p2_name", "p2_score", "p2_level", "winner"] # winner: 1, 2, tie, solo

# ---------- colors (cyan = P1 only, red = P2 only, gold = winner / rank 1) ----------
BG, PANEL, LINE = "#0A0A1F", "#12122B", "#26264A"
TEXT, MUTED = "#F2F2F7", "#9A9AB5"
P1, P2, GOLD, GREEN = "#00FFFF", "#FF4D4D", "#FFD700", "#3DDC84"
SILVER, BRONZE = "#C0C0D0", "#CD7F32"

PIXEL = "Consolas" # replaced by Press Start 2P when the font file loads


def pix(label, px, color=TEXT):
  """Style a label with the pixel font. Pixel font is used only for logo, names, scores."""
  label.setStyleSheet(f"font-family:'{PIXEL}'; font-size:{px}px; color:{color};")


def plain(label, px=14, color=TEXT, weight=400):
  label.setStyleSheet(f"font-size:{px}px; color:{color}; font-weight:{weight};")


def now_str():
  return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


# =====================================================================
# Backend: serial reader, pairing P1/P2, CSV saving
# =====================================================================
class Backend(QObject):
  connection = Signal(bool)
  match = Signal(object) # dict describing the current match
  saved = Signal()    # a row was written to the CSV
  live = Signal(object)  # live score update while a game is being played

  def __init__(self):
    super().__init__()
    self.live_games = {} # game id -> {"mode", "players": {1: entry, 2: entry}, "t"}
    self.pending = {}  # game id -> {1: entry, 2: entry, "t": time}
    self.lock = threading.Lock()
    self.auto_game_id = 1000

  @staticmethod
  def roast(p1, p2, winner):
    if winner == "solo":
      return "Not bad! Now bring a friend to beat."
    if winner == "tie":
      return "A tie? Nobody wins, nobody sleeps."
    w, l = (p1, p2) if winner == 1 else (p2, p1)
    if l["score"] * 2 < w["score"]:
      return f"{w['name']} dominated. {l['name']} never stood a chance."
    if w["score"] - l["score"] < max(1, w["score"] // 20):
      return "That was close! Rematch?"
    return f"{w['name']} made that look easy."

  @staticmethod
  def save_row(row):
    new = not os.path.exists(CSV_FILE)
    with open(CSV_FILE, "a", newline="") as f:
      w = csv.DictWriter(f, fieldnames=FIELDS)
      if new:
        w.writeheader()
      w.writerow(row)

  @staticmethod
  def blank_row(game, mode):
    return {"time": now_str(), "game": game, "mode": mode, "p1_name": "", "p1_score": "",
        "p1_level": "", "p2_name": "", "p2_score": "", "p2_level": "", "winner": ""}

  def handle_live(self, d):
    """Score update sent while the game is still running (before the name is entered)."""
    player = int(d["player"])
    game = int(d.get("game", 0))
    mode = d.get("mode", "1P")
    entry = {"player": player, "name": f"PLAYER {player}",
         "score": int(d.get("score", 0)), "level": int(d.get("level", 1)),
         "won": False}
    with self.lock:
      for old in [k for k, v in self.live_games.items() if time.time() - v["t"] > 600]:
        del self.live_games[old]
      g = self.live_games.get(game)
      new = g is None
      if new:
        g = self.live_games[game] = {"mode": mode, "players": {}, "t": 0}
      g["t"] = time.time()
      g["players"][player] = entry
      players = dict(g["players"])
    self.live.emit({"game": game, "mode": mode, "winner": None, "live": True,
            "new": new, "players": players, "roast": ""})

  def handle_score(self, d):
    player = int(d["player"])
    game = int(d.get("game", self.auto_game_id))
    mode = d.get("mode", "1P")
    entry = {"player": player, "name": (d.get("name") or "").strip() or "???",
         "score": int(d["score"]), "level": int(d.get("level", 1)),
         "won": bool(int(d.get("won", 0)))}

    with self.lock:
      self.live_games.pop(game, None) # the game is over, stop tracking it live
      if mode == "1P":
        row = self.blank_row(game, "1P")
        k = f"p{player}"
        row[f"{k}_name"], row[f"{k}_score"], row[f"{k}_level"] = (
          entry["name"], entry["score"], entry["level"])
        row["winner"] = "solo"
        self.save_row(row)
        self.match.emit({"game": game, "mode": "1P", "winner": "solo",
                 "players": {player: entry},
                 "roast": self.roast(entry, entry, "solo")})
        self.saved.emit()
        return

      g = self.pending.setdefault(game, {"t": time.time()})
      g[player] = entry
      for old in [k for k, v in self.pending.items() if time.time() - v["t"] > 600]:
        del self.pending[old]

      if 1 in g and 2 in g:
        p1, p2 = g[1], g[2]
        if p1["won"] and not p2["won"]:
          winner = 1
        elif p2["won"] and not p1["won"]:
          winner = 2
        elif p1["score"] != p2["score"]:
          winner = 1 if p1["score"] > p2["score"] else 2
        else:
          winner = "tie"
        row = self.blank_row(game, "2P")
        for k, e in (("p1", p1), ("p2", p2)):
          row[f"{k}_name"], row[f"{k}_score"], row[f"{k}_level"] = (
            e["name"], e["score"], e["level"])
        row["winner"] = winner
        self.save_row(row)
        del self.pending[game]
        self.match.emit({"game": game, "mode": "2P", "winner": winner,
                 "players": {1: p1, 2: p2},
                 "roast": self.roast(p1, p2, winner)})
        self.saved.emit()
      else: # one player is still typing a name
        self.match.emit({"game": game, "mode": "2P", "winner": None,
                 "players": {k: v for k, v in g.items() if k in (1, 2)},
                 "roast": ""})

  # ----- serial -----
  @staticmethod
  def find_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
      if any(k in p.description for k in ("Arduino", "CH340", "USB Serial")):
        return p.device
    return ports[0].device if ports else None

  def run_serial(self):
    while True:
      port = self.find_port()
      if not port:
        self.connection.emit(False)
        time.sleep(2)
        continue
      try:
        ser = serial.Serial(port, BAUD, timeout=0.2)
        time.sleep(2) # the Arduino resets when the port opens
        with self.lock:
          self.live_games.clear()
        self.connection.emit(True)
        while True:
          line = ser.readline().decode(errors="ignore").strip()
          if not line:
            continue
          if "PING" in line:
            ser.write(b"PONG\n")
          elif line.startswith("{"):
            try:
              d = json.loads(line)
              ev = d.get("event")
              if ev == "LIVE":
                self.handle_live(d)
              elif ev == "SCORE_SUBMIT":
                self.handle_score(d)
            except (ValueError, KeyError):
              print("Bad data:", line)
          else:
            # Flexible CSV parser: player,name,score,level,won
            # Example from Arduino: Serial.println("1,MAYA,75000,5,1");
            try:
              parts = [p.strip() for p in line.split(",")]
              if len(parts) >= 5:
                self.auto_game_id += 1
                d = {
                  "player": int(parts[0]),
                  "name": parts[1],
                  "score": int(parts[2]),
                  "level": int(parts[3]),
                  "won": int(parts[4]),
                  "game": self.auto_game_id,
                  "mode": "1P"
                }
                self.handle_score(d)
            except Exception as e:
              print("Ignored line:", line, e)
      except serial.SerialException as e:
        print("Serial lost:", e)
        self.connection.emit(False)
        time.sleep(2)

  def run_demo(self):
    self.connection.emit(True)
    names = ["MAYA", "ALEX", "JUN", "SAM"]
    game = 100
    while True:
      time.sleep(8)
      game += 1
      if random.random() < 0.25:
        self.handle_score({"player": 1, "name": random.choice(names),
                  "score": random.randint(20000, 150000),
                  "level": random.randint(1, 10), "game": game,
                  "mode": "1P", "won": 0})
        continue
      a, b = random.sample(names, 2)
      w1 = random.randint(0, 1)
      s = {1: 0, 2: 0}
      for step in range(8):     # fake live scoring
        for p in (1, 2):
          s[p] += random.randint(0, 20000)
          self.handle_live({"player": p, "game": game, "mode": "2P",
                   "score": s[p], "level": 1 + step // 2})
        time.sleep(1)
      self.handle_score({"player": 1, "name": a, "score": s[1],
                "level": 5, "game": game, "mode": "2P", "won": w1})
      time.sleep(3)
      self.handle_score({"player": 2, "name": b, "score": s[2],
                "level": 5, "game": game, "mode": "2P", "won": 1 - w1})


def read_matches():
  if not os.path.exists(CSV_FILE):
    return []
  with open(CSV_FILE, newline="") as f:
    return list(csv.DictReader(f))


# =====================================================================
# UI pieces
# =====================================================================
class PlayerCard(QFrame):
  def __init__(self, num):
    super().__init__()
    self.num = num
    self.color = P1 if num == 1 else P2
    self.setObjectName("card")
    self.setMinimumHeight(270)

    v = QVBoxLayout(self)
    v.setContentsMargins(24, 20, 24, 24)
    v.setSpacing(12)

    top = QHBoxLayout()
    self.tag = QLabel(f"Player {num}")
    plain(self.tag, 13, self.color, 600)
    self.level = QLabel("")
    self.level.setStyleSheet(f"background:#23234A; border-radius:4px; padding:4px 8px;"
                 f" font-size:12px; font-weight:600; color:{TEXT};")
    top.addWidget(self.tag)
    top.addStretch()
    top.addWidget(self.level)

    self.name = QLabel("")
    self.name.setAlignment(Qt.AlignCenter)
    self.score = QLabel("")
    self.score.setAlignment(Qt.AlignCenter)
    self.status = QLabel("")
    self.status.setAlignment(Qt.AlignCenter)

    v.addLayout(top)
    v.addStretch()
    v.addWidget(self.name)
    v.addWidget(self.score)
    v.addStretch()
    v.addWidget(self.status)

    self._target, self._step = 0, 0
    self._shown, self._start = 0, 0
    self._timer = QTimer(self)
    self._timer.timeout.connect(self._tick)
    self._frame(PANEL, LINE, 1, False)

  def _frame(self, bg, border, width, dashed, glow=False):
    style = "dashed" if dashed else "solid"
    self.setStyleSheet(f"#card {{ background:{bg}; border:{width}px {style} {border};"
              f" border-radius:8px; }}")
    if glow:
      fx = QGraphicsDropShadowEffect(self)
      fx.setBlurRadius(36)
      fx.setOffset(0, 0)
      fx.setColor(QColor(GOLD))
      self.setGraphicsEffect(fx)
    else:
      self.setGraphicsEffect(None)

  def _tick(self):
    self._step += 1
    t = min(1.0, self._step / 25)
    self._shown = int(self._start + (self._target - self._start) * (1 - (1 - t) ** 3))
    self.score.setText(f"{self._shown:,}")
    if t >= 1.0:
      self._timer.stop()

  def reset_score(self):
    self._timer.stop()
    self._target = self._shown = self._start = 0

  def set_ghost(self, text):
    self._timer.stop()
    self._target = self._shown = self._start = 0
    self._frame("transparent", "#3A3A60", 1, True)
    self.level.hide()
    plain(self.name, 16, MUTED)
    self.name.setText(text)
    self.score.setText("")
    self.status.setText("")

  def set_entry(self, e, outcome):
    """outcome: pending | winner | loser | tie | solo"""
    if outcome == "winner":
      self._frame(PANEL, GOLD, 2, False, glow=True)
    else:
      self._frame(PANEL, LINE, 1, False)
    dim = outcome == "loser"
    self.level.setText(f"Lv {e['level']}")
    self.level.show()
    pix(self.name, 22, "#B8B8D0" if dim else TEXT)
    self.name.setText(e["name"])
    score_color = GOLD if outcome == "winner" else ("#B8B8D0" if dim else TEXT)
    pix(self.score, 28, score_color)
    if e["score"] != self._target:
      # count up from whatever is currently shown, so live updates feel smooth
      self._start, self._target, self._step = self._shown, e["score"], 0
      self._timer.start(16)

    texts = {"live": ("\u25CF LIVE", GREEN),
         "winner": ("\u2605 WINNER", GOLD), "loser": ("LOSER", "#B8B8D0"),
         "tie": ("TIE", TEXT), "solo": ("SOLO RUN", MUTED),
         "pending": ("Waiting for the other player", MUTED)}
    t, c = texts[outcome]
    plain(self.status, 14, c, 700)
    self.status.setText(t)


class MatchPage(QWidget):
  next_clicked = Signal()

  def __init__(self):
    super().__init__()
    self.match = None
    v = QVBoxLayout(self)
    v.setContentsMargins(0, 32, 0, 32)
    v.setSpacing(24)

    head = QHBoxLayout()
    col = QVBoxLayout()
    col.setSpacing(6)
    self.title = QLabel("")
    self.subtitle = QLabel("")
    plain(self.subtitle, 14, MUTED)
    col.addWidget(self.title)
    col.addWidget(self.subtitle)
    self.mode = QLabel("")
    self.mode.setStyleSheet(f"background:#23234A; border-radius:4px; padding:4px 10px;"
                f" font-size:12px; font-weight:600; color:{TEXT};")
    head.addLayout(col)
    head.addStretch()
    head.addWidget(self.mode, 0, Qt.AlignTop)
    v.addLayout(head)

    self.cards = QHBoxLayout()
    self.cards.setSpacing(24)
    self.c1, self.c2 = PlayerCard(1), PlayerCard(2)
    self.cards.addStretch(0)
    self.cards.addWidget(self.c1, 1)
    self.cards.addWidget(self.c2, 1)
    self.cards.addStretch(0)
    v.addLayout(self.cards, 1)

    bar = QFrame()
    bar.setObjectName("bar")
    bar.setStyleSheet(f"#bar {{ background:{PANEL}; border:1px solid {LINE}; border-radius:8px; }}")
    h = QHBoxLayout(bar)
    h.setContentsMargins(24, 16, 24, 16)
    h.setSpacing(16)
    col2 = QVBoxLayout()
    cap = QLabel("Post-game commentary")
    plain(cap, 12, MUTED)
    self.roast = QLabel("")
    self.roast.setWordWrap(True)
    plain(self.roast, 16, TEXT, 500)
    col2.addWidget(cap)
    col2.addWidget(self.roast)
    self.roast_on = QCheckBox("Roast mode")
    self.roast_on.setChecked(True)
    self.roast_on.setStyleSheet(f"font-size:13px; color:{TEXT}; spacing:8px;")
    self.roast_on.toggled.connect(self.refresh_roast)
    nxt = QPushButton("Next match")
    nxt.setCursor(Qt.PointingHandCursor)
    nxt.setStyleSheet(f"QPushButton {{ background:{TEXT}; color:{BG}; border:none; border-radius:6px;"
             f" padding:12px 20px; font-size:14px; font-weight:600; }}"
             f"QPushButton:hover {{ background:#FFFFFF; }}")
    nxt.clicked.connect(self.next_clicked.emit)
    h.addLayout(col2, 1)
    h.addWidget(self.roast_on)
    h.addWidget(nxt)
    v.addWidget(bar)

    self._pulse = False
    self._pulse_timer = QTimer(self)
    self._pulse_timer.timeout.connect(self._do_pulse)
    self._pulse_timer.start(700)
    self.show_waiting()

  def _stretches(self, solo):
    self.cards.setStretch(0, 1 if solo else 0)
    self.cards.setStretch(3, 1 if solo else 0)
    for c in (self.c1, self.c2):
      c.setMaximumWidth(460 if solo else 16777215)

  def _do_pulse(self):
    if self.match is None:
      self._pulse = not self._pulse
      pix(self.title, 18, TEXT if self._pulse else MUTED)

  def refresh_roast(self):
    m = self.match
    if m is None or m["winner"] is None:
      self.roast.setText("Commentary appears when the match ends.")
    elif self.roast_on.isChecked():
      self.roast.setText(m["roast"])
    else:
      self.roast.setText("Roast mode is off.")

  def show_waiting(self):
    self.match = None
    self._game = None
    pix(self.title, 18, TEXT)
    self.title.setText("WAITING FOR PLAYERS")
    self.subtitle.setText("Start a game on the LED matrix to begin.")
    self.mode.hide()
    self._stretches(False)
    self.c1.show()
    self.c2.show()
    self.c1.set_ghost("Not joined")
    self.c2.set_ghost("Not joined")
    self.refresh_roast()

  def show_match(self, m):
    self.match = m
    live = bool(m.get("live"))
    if getattr(self, "_game", None) != m["game"]:
      self._game = m["game"]     # new game: restart the score counters from 0
      self.c1.reset_score()
      self.c2.reset_score()
    plain(self.title, 24, TEXT, 600)
    self.title.setText(f"Game #{m['game']}")
    self.mode.setText(m["mode"])
    self.mode.show()
    players, w = m["players"], m["winner"]

    if m["mode"] == "1P" or w == "solo":
      self.subtitle.setText("Game in progress" if live else "Solo run")
      only = next(iter(players))
      self._stretches(True)
      card, other = (self.c1, self.c2) if only == 1 else (self.c2, self.c1)
      other.hide()
      card.show()
      card.set_entry(players[only], "live" if live else "solo")
    else:
      self._stretches(False)
      self.c1.show()
      self.c2.show()
      if live:
        self.subtitle.setText("Match in progress")
        for n, card in ((1, self.c1), (2, self.c2)):
          if n in players:
            card.set_entry(players[n], "live")
          else:
            card.set_ghost("Joining...")
        self.refresh_roast()
        return
      if w is None:
        self.subtitle.setText("One player is still entering a name.")
      elif w == "tie":
        self.subtitle.setText("Match complete.")
      else:
        self.subtitle.setText("Match complete.")
      for n, card in ((1, self.c1), (2, self.c2)):
        if n not in players:
          card.set_ghost("Entering name...")
          continue
        if w is None:
          card.set_entry(players[n], "pending")
        elif w == "tie":
          card.set_entry(players[n], "tie")
        else:
          card.set_entry(players[n], "winner" if w == n else "loser")
    self.refresh_roast()


def make_table(headers, widths, right_cols=()):
  t = QTableWidget(0, len(headers))
  t.setHorizontalHeaderLabels(headers)
  for c in range(len(headers)):
    t.horizontalHeaderItem(c).setTextAlignment(
      (Qt.AlignRight if c in right_cols else Qt.AlignLeft) | Qt.AlignVCenter)
  t.horizontalHeader().setDefaultAlignment(Qt.AlignLeft | Qt.AlignVCenter)
  t.verticalHeader().setVisible(False)
  t.verticalHeader().setDefaultSectionSize(48)
  t.setShowGrid(False)
  t.setSelectionMode(QAbstractItemView.NoSelection)
  t.setEditTriggers(QAbstractItemView.NoEditTriggers)
  t.setFocusPolicy(Qt.NoFocus)
  t.horizontalHeader().setHighlightSections(False)
  t.horizontalHeader().setStretchLastSection(True)
  t.horizontalHeader().setFixedHeight(40)
  for i, w in enumerate(widths):
    t.setColumnWidth(i, w)
  t.setStyleSheet(
    f"QTableWidget {{ background:{PANEL}; border:1px solid {LINE}; border-radius:8px; font-size:14px; }}"
    f"QHeaderView {{ background:{PANEL}; border:none; }}"
    f"QTableCornerButton::section {{ background:{PANEL}; border:none; }}"
    f"QHeaderView::section {{ background:{PANEL}; color:{MUTED}; border:none;"
    f" border-bottom:1px solid {LINE}; padding:0 12px; font-size:12px; font-weight:600; text-align:left; }}"
    f"QTableWidget::item {{ border-bottom:1px solid #1C1C3A; padding:0 12px; }}"
    "QScrollBar:vertical { background:transparent; width:10px; margin:2px; }"
    "QScrollBar::handle:vertical { background:#2F2F55; border-radius:3px; min-height:24px; }"
    "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { }"
    "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }")
  return t


def cell(text, color=TEXT, align=Qt.AlignLeft, pixel=0, bold=False):
  it = QTableWidgetItem(text)
  it.setForeground(QColor(color))
  it.setTextAlignment(align | Qt.AlignVCenter)
  f = QFont(PIXEL) if pixel else QFont()
  if pixel:
    f.setPixelSize(pixel)
  else:
    f.setPixelSize(14)
    f.setBold(bold)
  it.setFont(f)
  return it


def page_header(title, subtitle):
  box = QVBoxLayout()
  box.setSpacing(6)
  a, b = QLabel(title), QLabel(subtitle)
  plain(a, 24, TEXT, 600)
  plain(b, 14, MUTED)
  box.addWidget(a)
  box.addWidget(b)
  return box


def empty_label(text):
  lab = QLabel(text)
  lab.setAlignment(Qt.AlignCenter)
  plain(lab, 14, MUTED)
  return lab


class LeaderboardPage(QWidget):
  def __init__(self):
    super().__init__()
    v = QVBoxLayout(self)
    v.setContentsMargins(0, 32, 0, 32)
    v.setSpacing(24)
    v.addLayout(page_header("Leaderboard", "The ten highest scores recorded on this machine."))
    self.table = make_table(["Rank", "Name", "Score", "Level", "Date"], [70, 180, 170, 90, 100], right_cols=(2,))
    self.empty = empty_label("No scores yet. Play a game!")
    v.addWidget(self.table)
    v.addWidget(self.empty)
    v.addStretch(1)

  def refresh(self, matches):
    rows = []
    for m in matches:
      for k in ("p1", "p2"):
        if m[f"{k}_name"]:
          rows.append((m[f"{k}_name"], int(m[f"{k}_score"]), int(m[f"{k}_level"]), m["time"]))
    rows.sort(key=lambda r: r[1], reverse=True)
    rows = rows[:10]
    self.empty.setVisible(not rows)
    self.table.setVisible(bool(rows))
    self.table.setRowCount(len(rows))
    self.table.setFixedHeight(40 + 48 * len(rows) + 2)
    medal = {0: GOLD, 1: SILVER, 2: BRONZE}
    R = Qt.AlignRight
    for i, (name, score, lvl, when) in enumerate(rows):
      date = datetime.strptime(when, "%Y-%m-%d %H:%M:%S").strftime("%b %d")
      self.table.setItem(i, 0, cell(str(i + 1), medal.get(i, MUTED), bold=True))
      self.table.setItem(i, 1, cell(name, bold=True))
      self.table.setItem(i, 2, cell(f"{score:,}", TEXT, R, pixel=13))
      self.table.setItem(i, 3, cell(f"Lv {lvl}", MUTED))
      self.table.setItem(i, 4, cell(date, MUTED))


class RecordsPage(QWidget):
  def __init__(self):
    super().__init__()
    v = QVBoxLayout(self)
    v.setContentsMargins(0, 32, 0, 32)
    v.setSpacing(24)
    v.addLayout(page_header("Player records", "Results grouped by player name, most wins first."))
    self.grid = QGridLayout()
    self.grid.setSpacing(16)
    self.empty = empty_label("No records yet. Play a game!")
    v.addLayout(self.grid)
    v.addWidget(self.empty)
    v.addStretch(1)

  def refresh(self, matches):
    rec = {}
    for m in matches:
      for i, k in ((1, "p1"), (2, "p2")):
        name = m[f"{k}_name"].upper()
        if not name:
          continue
        r = rec.setdefault(name, {"name": name, "w": 0, "l": 0, "t": 0, "best": 0})
        r["best"] = max(r["best"], int(m[f"{k}_score"]))
        if m["winner"] == "tie":
          r["t"] += 1
        elif m["winner"] in ("1", "2"):
          r["w" if int(m["winner"]) == i else "l"] += 1
    while self.grid.count():
      w = self.grid.takeAt(0).widget()
      if w:
        w.hide()
        w.setParent(None)
        w.deleteLater()
    data = sorted(rec.values(), key=lambda r: r["w"], reverse=True)
    self.empty.setVisible(not data)
    for n, r in enumerate(data):
      self.grid.addWidget(self.card(r), n // 3, n % 3)
    for c in range(3):
      self.grid.setColumnStretch(c, 1)

  def card(self, r):
    f = QFrame()
    f.setObjectName("rc")
    f.setStyleSheet(f"#rc {{ background:{PANEL}; border:1px solid {LINE}; border-radius:8px; }}")
    v = QVBoxLayout(f)
    v.setContentsMargins(20, 18, 20, 18)
    v.setSpacing(4)
    name = QLabel(r["name"])
    pix(name, 16)
    best = QLabel(f"Best {r['best']:,}")
    plain(best, 12, MUTED)
    v.addWidget(name)
    v.addWidget(best)
    v.addSpacing(12)
    stats = QHBoxLayout()
    for val, lab in ((r["w"], "Wins"), (r["l"], "Losses"), (r["t"], "Ties")):
      col = QVBoxLayout()
      col.setSpacing(2)
      a, b = QLabel(str(val)), QLabel(lab)
      plain(a, 24, TEXT, 600)
      plain(b, 12, MUTED)
      col.addWidget(a)
      col.addWidget(b)
      stats.addLayout(col)
    v.addLayout(stats)
    v.addSpacing(12)
    total = r["w"] + r["l"] + r["t"]
    rate = round(100 * r["w"] / total) if total else 0
    row = QHBoxLayout()
    a, b = QLabel("Win rate"), QLabel(f"{rate}%")
    plain(a, 12, MUTED)
    plain(b, 12, TEXT, 600)
    row.addWidget(a)
    row.addStretch()
    row.addWidget(b)
    v.addLayout(row)
    bar = QProgressBar()
    bar.setRange(0, 100)
    bar.setValue(rate)
    bar.setTextVisible(False)
    bar.setFixedHeight(6)
    bar.setStyleSheet(f"QProgressBar {{ background:#2A2A48; border:none; border-radius:3px; }}"
             f"QProgressBar::chunk {{ background:{GREEN}; border-radius:3px; }}")
    v.addWidget(bar)
    return f


class HistoryPage(QWidget):
  def __init__(self):
    super().__init__()
    v = QVBoxLayout(self)
    v.setContentsMargins(0, 32, 0, 32)
    v.setSpacing(24)
    v.addLayout(page_header("Recent matches", "Newest first. The winner is shown in white."))
    self.table = make_table(["Time", "P1 name", "P1 score", "P2 score", "P2 name", "Winner"],
                [90, 130, 120, 120, 130, 100], right_cols=(2, 3))
    self.table.verticalHeader().setDefaultSectionSize(56)
    self.empty = empty_label("No matches yet. Play a game!")
    v.addWidget(self.table, 1)
    v.addWidget(self.empty)

  def refresh(self, matches):
    rows = list(reversed(matches))[:50]
    self.empty.setVisible(not rows)
    self.table.setVisible(bool(rows))
    self.table.setRowCount(len(rows))
    R = Qt.AlignRight
    for i, m in enumerate(rows):
      w = m["winner"]
      t = datetime.strptime(m["time"], "%Y-%m-%d %H:%M:%S").strftime("%H:%M")
      self.table.setItem(i, 0, cell(t, MUTED))

      def side(n):
        on = w == str(n) or w in ("tie", "solo")
        return TEXT if on else MUTED, w == str(n)

      for n, ncol, scol in ((1, 1, 2), (2, 4, 3)):
        name, score = m[f"p{n}_name"], m[f"p{n}_score"]
        color, crown = side(n)
        label = ("\u2605 " if crown else "") + name if name else "\u2014"
        self.table.setItem(i, ncol, cell(label, color, bold=crown))
        self.table.setItem(i, scol, cell(f"{int(score):,}" if score != "" else "",
                        color, R, pixel=12))
      if w == "tie":
        res = "Tie"
      elif w == "solo":
        res = "Solo"
      else:
        res = m[f"p{w}_name"]
      self.table.setItem(i, 5, cell(res, MUTED))


class Centered(QWidget):
  """Keeps page content in a centered column instead of stretching edge to edge."""
  def __init__(self, page, max_width):
    super().__init__()
    h = QHBoxLayout(self)
    h.setContentsMargins(24, 0, 24, 0)
    h.addStretch(1)
    page.setMaximumWidth(max_width)
    h.addWidget(page, 100)
    h.addStretch(1)


class MainWindow(QMainWindow):
  def __init__(self, backend):
    super().__init__()
    self.setWindowTitle("Tetris Battle")
    self.resize(1100, 780)
    self.setMinimumSize(980, 700)

    root = QWidget()
    root.setObjectName("root")
    root.setStyleSheet(f"#root {{ background:{BG}; }}")
    self.setCentralWidget(root)
    outer = QVBoxLayout(root)
    outer.setContentsMargins(0, 0, 0, 0)
    outer.setSpacing(0)

    # --- header ---
    header = QFrame()
    header.setObjectName("hdr")
    header.setStyleSheet(f"#hdr {{ border-bottom:1px solid {LINE}; }}")
    header.setFixedHeight(64)
    hl = QHBoxLayout(header)
    hl.setContentsMargins(32, 0, 32, 0)
    logo = QLabel('TETRIS <span style="color:#00FFFF">BATTLE</span>')
    logo.setStyleSheet(f"font-family:'{PIXEL}'; font-size:14px; color:{TEXT};")
    hl.addWidget(logo)
    hl.addStretch(1)

    self.tabs = []
    names = ["Match", "Leaderboard", "Records", "History"]
    for i, n in enumerate(names):
      b = QPushButton(n)
      b.setCheckable(True)
      b.setCursor(Qt.PointingHandCursor)
      b.setFixedHeight(64)
      b.setStyleSheet(
        f"QPushButton {{ background:transparent; border:none; border-bottom:2px solid transparent;"
        f" color:{MUTED}; font-size:14px; font-weight:500; padding:0 16px; }}"
        f"QPushButton:hover {{ color:{TEXT}; }}"
        f"QPushButton:checked {{ color:{TEXT}; border-bottom:2px solid {TEXT}; }}")
      b.clicked.connect(lambda _=False, k=i: self.go(k))
      self.tabs.append(b)
      hl.addWidget(b)
    hl.addStretch(1)
    self.status = QLabel("")
    hl.addWidget(self.status)
    outer.addWidget(header)

    # --- pages ---
    self.match_page = MatchPage()
    self.board = LeaderboardPage()
    self.records = RecordsPage()
    self.history = HistoryPage()
    self.stack = QStackedWidget()
    for page, w in ((self.match_page, 960), (self.board, 760),
            (self.records, 960), (self.history, 760)):
      self.stack.addWidget(Centered(page, w))
    outer.addWidget(self.stack, 1)

    self.match_page.next_clicked.connect(self.match_page.show_waiting)
    backend.connection.connect(self.on_connection)
    backend.match.connect(self.on_match)
    backend.live.connect(self.on_live)
    backend.saved.connect(self.refresh_data)

    self.on_connection(False)
    self.refresh_data()
    self.go(0)

  def go(self, i):
    self.stack.setCurrentIndex(i)
    for k, b in enumerate(self.tabs):
      b.setChecked(k == i)

  def on_connection(self, ok):
    color = GREEN if ok else P2
    text = "Arduino connected" if ok else "Arduino disconnected"
    self.status.setText(f'<span style="color:{color}">\u25CF</span> {text}')
    plain(self.status, 13, MUTED)

  def on_live(self, m):
    self.match_page.show_match(m)
    if m.get("new"):     # jump to the Match tab only when a game starts
      self.go(0)

  def on_match(self, m):
    self.match_page.show_match(m)
    self.go(0)

  def refresh_data(self):
    matches = read_matches()
    self.board.refresh(matches)
    self.records.refresh(matches)
    self.history.refresh(matches)


def main():
  global PIXEL
  ap = argparse.ArgumentParser()
  ap.add_argument("--demo", action="store_true", help="fake games, no Arduino needed")
  args, qt_args = ap.parse_known_args()

  app = QApplication([sys.argv[0]] + qt_args)
  app.setFont(QFont("Segoe UI", 10))
  fid = QFontDatabase.addApplicationFont(FONT_FILE)
  if fid != -1:
    PIXEL = QFontDatabase.applicationFontFamilies(fid)[0]
  else:
    print("Pixel font not found at", FONT_FILE, "- using Consolas")
  app.setStyleSheet(f"QWidget {{ color:{TEXT}; }} QLabel {{ background:transparent; }}")

  backend = Backend()
  win = MainWindow(backend)
  win.show()
  threading.Thread(target=backend.run_demo if args.demo else backend.run_serial,
           daemon=True).start()
  sys.exit(app.exec())


if __name__ == "__main__":
  main()