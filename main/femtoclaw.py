#!/usr/bin/env python3
"""
femtoclaw.py
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
FemtoClaw MCU Terminal
  • Detects USB serial devices and shows them in a live table.
  • Lets you pick a port, load a firmware .bin / .uf2, and flash it.
  • Opens UART terminal.
  • Config tab covers WiFi, LLM provider, Telegram bot, Discord bot
  • Control tab covers Board Hardware Controlling using .md file

Install:  pip install pyserial esptool platformio PyQt6
Run    :  python femtoclaw.py
Developed by : Al Mahmud Samiul
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
"""

import sys, os, threading, time, subprocess, shutil, json, re, sysconfig, base64

from PyQt6.QtCore import (Qt, QThread, pyqtSignal, QTimer, QObject, pyqtSlot)
from PyQt6.QtGui import (QColor, QFont, QTextCursor, QTextCharFormat,
                          QPalette, QKeySequence)
from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QFrame, QLabel, QPushButton,
    QComboBox, QLineEdit, QTextEdit, QPlainTextEdit, QCheckBox, QSpinBox,
    QDoubleSpinBox, QSlider, QProgressBar, QTabWidget, QTreeWidget,
    QTreeWidgetItem, QListWidget, QGroupBox, QFileDialog, QMessageBox,
    QVBoxLayout, QHBoxLayout, QGridLayout, QFormLayout, QSizePolicy,
    QSplitter, QScrollArea, QSpacerItem
)

try:
    import serial
    import serial.tools.list_ports
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False


# ── Tool detection ────────────────────────────────────────────────────────────
def _scripts_dirs() -> list[str]:
    """
    Return all candidate directories that may contain CLI tools like 'pio'.
    Covers: current venv, the Python interpreter's own Scripts/bin dir,
    pipx installs, user-level pip installs, and the system PATH.
    """
    dirs: list[str] = []

    # 1. The Scripts/bin folder of whichever Python is running this script.
    exe_dir = os.path.dirname(sys.executable)
    dirs.append(exe_dir)

    # 2. sysconfig 'scripts' path for the current interpreter
    try:
        dirs.append(sysconfig.get_path("scripts"))
    except Exception:
        pass

    # 3. VIRTUAL_ENV / CONDA_PREFIX env var (set by PyCharm / activate scripts)
    venv = os.environ.get("VIRTUAL_ENV") or os.environ.get("CONDA_PREFIX")
    if venv:
        dirs.append(os.path.join(venv, "Scripts"))   # Windows
        dirs.append(os.path.join(venv, "bin"))       # macOS / Linux

    # 4. pipx installs and common user-level locations
    home = os.path.expanduser("~")
    dirs += [
        os.path.join(home, ".local", "pipx", "venvs", "platformio",
                     "Scripts" if sys.platform == "win32" else "bin"),
        os.path.join(home, ".local", "bin"),
        os.path.join(home, "AppData", "Roaming", "Python",
                     f"Python{sys.version_info.major}{sys.version_info.minor}",
                     "Scripts"),
        # PlatformIO standalone installer default
        os.path.join(home, ".platformio", "penv", "Scripts"),
        os.path.join(home, ".platformio", "penv", "bin"),
    ]

    # 5. Everything already on PATH
    dirs += os.environ.get("PATH", "").split(os.pathsep)

    # Deduplicate while preserving order, skip None/empty
    seen: set[str] = set()
    result: list[str] = []
    for d in dirs:
        if d and d not in seen:
            seen.add(d)
            result.append(d)
    return result


def _find_tool(*names: str) -> str | None:
    """Search all candidate script directories for any of the given tool names."""
    exts = ["", ".exe", ".cmd", ".bat"] if sys.platform == "win32" else [""]
    for d in _scripts_dirs():
        for name in names:
            for ext in exts:
                candidate = os.path.join(d, name + ext)
                if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
                    return candidate
    # Final fallback: system shutil.which
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return None


def find_pio() -> str | None:
    return _find_tool("pio", "platformio")


def find_esptool() -> str | None:
    return _find_tool("esptool.py", "esptool")


# ── Constants ─────────────────────────────────────────────────────────────────
APP  = "FemtoClaw MCU Terminal"
VER  = "1.1.0"
BAUDS = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600]
BAUD0 = 115200

# Palette
BG     = "#0d1117"
BGPAN  = "#161b22"
BGINP  = "#1c2128"
FG     = "#e6edf3"
FGDIM  = "#8b949e"
GREEN  = "#7ee787"
ORANGE = "#f0883e"
RED    = "#f85149"
BLUE   = "#58a6ff"
PURPLE = "#bc8cff"
YELLOW = "#e3b341"
CURSOR = "#7ee787"
MONO_FONT = "Cascadia Code" if sys.platform == "win32" else "Monospace"


def strip_ansi(t: str) -> str:
    return re.sub(r'\033\[[0-9;]*m', '', t)


WELCOME = r"""
  ███████╗███████╗███╗   ███╗████████╗ ██████╗  ██████╗██╗      █████╗ ██╗    ██╗
  ██╔════╝██╔════╝████╗ ████║╚══██╔══╝██╔═══██╗██╔════╝██║     ██╔══██╗██║    ██║
  █████╗  █████╗  ██╔████╔██║   ██║   ██║   ██║██║     ██║     ███████║██║ █╗ ██║
  ██╔══╝  ██╔══╝  ██║╚██╔╝██║   ██║   ██║   ██║██║     ██║     ██╔══██║██║███╗██║
  ██║     ███████╗██║ ╚═╝ ██║   ██║   ╚██████╔╝╚██████╗███████╗██║  ██║╚███╔███╔╝
  ╚═╝     ╚══════╝╚═╝     ╚═╝   ╚═╝    ╚═════╝  ╚═════╝╚══════╝╚═╝  ╚═╝ ╚══╝╚══╝
  FemtoClaw — MCU AI Assistant with Telegram & Discord · amsamiul.dev@gmail.com
  ─────────────────────────────────────────────────────────────────────────────────
"""

# ── Global stylesheet ─────────────────────────────────────────────────────────
STYLE = f"""
QWidget {{
    background: {BG};
    color: {FG};
    font-family: "Segoe UI", sans-serif;
    font-size: 10pt;
}}
QFrame#banner {{
    background: {BGPAN};
    border-bottom: 1px solid #30363d;
}}
QTabWidget::pane {{
    border: none;
    background: {BG};
}}
QTabBar::tab {{
    background: {BGPAN};
    color: {FGDIM};
    padding: 8px 18px;
    border: none;
    font-size: 10pt;
}}
QTabBar::tab:selected {{
    background: {BGINP};
    color: {FG};
    border-bottom: 2px solid {PURPLE};
}}
QTabBar::tab:hover:!selected {{
    background: #1c2128;
}}
QPushButton {{
    background: {BGINP};
    color: {FG};
    border: 1px solid #30363d;
    border-radius: 5px;
    padding: 5px 14px;
    font-size: 10pt;
}}
QPushButton:hover {{ background: #2d333b; }}
QPushButton:pressed {{ background: {BGINP}; }}
QComboBox {{
    background: {BGINP};
    border: 1px solid #30363d;
    border-radius: 4px;
    padding: 4px 8px;
    color: {FG};
    min-height: 24px;
}}
QComboBox::drop-down {{ border: none; width: 20px; }}
QComboBox QAbstractItemView {{
    background: {BGINP};
    color: {FG};
    selection-background-color: {BLUE};
}}
QLineEdit {{
    background: {BGINP};
    border: 1px solid #30363d;
    border-radius: 4px;
    padding: 4px 8px;
    color: {FG};
    min-height: 24px;
}}
QLineEdit:focus {{
    border-color: {BLUE};
}}
QTextEdit, QPlainTextEdit {{
    background: {BG};
    border: none;
    color: {FG};
    font-family: "{MONO_FONT}", monospace;
    font-size: 11pt;
}}
QGroupBox {{
    border: 1px solid #30363d;
    border-radius: 6px;
    margin-top: 10px;
    padding-top: 6px;
    font-size: 9pt;
    color: {FGDIM};
}}
QGroupBox::title {{
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 10px;
    padding: 0 4px;
    color: {FGDIM};
}}
QTreeWidget {{
    background: {BGINP};
    alternate-background-color: #1a2030;
    border: 1px solid #30363d;
    border-radius: 4px;
    color: {FG};
    gridline-color: #30363d;
}}
QTreeWidget::item:selected {{
    background: #1f3458;
}}
QHeaderView::section {{
    background: {BGPAN};
    color: {FGDIM};
    border: none;
    border-right: 1px solid #30363d;
    padding: 4px 8px;
    font-size: 9pt;
}}
QListWidget {{
    background: {BGINP};
    border: 1px solid #30363d;
    border-radius: 4px;
    color: {FG};
}}
QListWidget::item:selected {{ background: {BLUE}; color: white; }}
QSpinBox, QDoubleSpinBox {{
    background: {BGINP};
    border: 1px solid #30363d;
    border-radius: 4px;
    padding: 3px 6px;
    color: {FG};
    min-height: 24px;
}}
QSlider::groove:horizontal {{
    height: 4px;
    background: #30363d;
    border-radius: 2px;
}}
QSlider::handle:horizontal {{
    background: {BLUE};
    width: 14px;
    height: 14px;
    margin: -5px 0;
    border-radius: 7px;
}}
QSlider::sub-page:horizontal {{ background: {BLUE}; border-radius: 2px; }}
QProgressBar {{
    background: {BGINP};
    border: 1px solid #30363d;
    border-radius: 3px;
    height: 8px;
    text-align: center;
}}
QProgressBar::chunk {{ background: {GREEN}; border-radius: 3px; }}
QScrollBar:vertical {{
    background: {BG};
    width: 8px;
    margin: 0;
}}
QScrollBar::handle:vertical {{
    background: #30363d;
    border-radius: 4px;
    min-height: 20px;
}}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height: 0; }}
QCheckBox {{ color: {FG}; spacing: 6px; }}
QCheckBox::indicator {{
    width: 15px; height: 15px;
    border: 1px solid #30363d;
    border-radius: 3px;
    background: {BGINP};
}}
QCheckBox::indicator:checked {{ background: {GREEN}; border-color: {GREEN}; }}
"""

# ── Button style ──────────────────────────────────────────────────────
_BTN_BASE = "border-radius:5px; padding:5px 14px; font-size:10pt; border:none;"
BTN_STYLE = {
    "green": f"QPushButton{{ background:#238636; color:white; {_BTN_BASE} }}"
             f"QPushButton:hover{{ background:#2ea043; }}"
             f"QPushButton:pressed{{ background:#1a7f37; }}",
    "red":   f"QPushButton{{ background:#b62324; color:white; {_BTN_BASE} }}"
             f"QPushButton:hover{{ background:#d73a3a; }}"
             f"QPushButton:pressed{{ background:#8b1a1a; }}",
    "blue":  f"QPushButton{{ background:#1158c7; color:white; {_BTN_BASE} }}"
             f"QPushButton:hover{{ background:#316dca; }}"
             f"QPushButton:pressed{{ background:#0d419d; }}",
    "tg":    f"QPushButton{{ background:#229ED9; color:white; {_BTN_BASE} }}"
             f"QPushButton:hover{{ background:#32B0E9; }}"
             f"QPushButton:pressed{{ background:#1882B9; }}",
    "orange":f"QPushButton{{ background:#9a6700; color:white; {_BTN_BASE} }}"
             f"QPushButton:hover{{ background:#b07800; }}"
             f"QPushButton:pressed{{ background:#7a5200; }}",
    "dc":    f"QPushButton{{ background:#5865F2; color:white; {_BTN_BASE} }}"
             f"QPushButton:hover{{ background:#6875F5; }}"
             f"QPushButton:pressed{{ background:#4752C4; }}",
}

def styled_btn(label: str, style: str, parent=None) -> "QPushButton":
    btn = QPushButton(label, parent)
    btn.setStyleSheet(BTN_STYLE[style])
    return btn


# ── tag colour map ─────────────────────────────────────
TAG_COLORS = {
    "banner": PURPLE,
    "prompt": GREEN,
    "user":   BLUE,
    "info":   BLUE,
    "ok":     GREEN,
    "err":    RED,
    "warn":   ORANGE,
    "dim":    FGDIM,
    "agent":  PURPLE,
    "tool":   ORANGE,
    "tg":     "#229ED9",
    "dc":     "#5865F2",
    "board":  "#7ee787",
    "action": "#f0883e",
    "result": "#bc8cff",
}


# ── serial reader ─────────────────────────────────────────────────────
class SerialSignals(QObject):
    line_received = pyqtSignal(str)

class CompileSignals(QObject):
    """Signals emitted from the compile/flash background threads."""
    progress_show  = pyqtSignal(bool, str)
    progress_pct   = pyqtSignal(int, str)
    compile_btn_state = pyqtSignal(bool)   # True = compiling, False = idle

class BoardPushSignals(QObject):
    """Thread-safe signals for the board-push background thread."""
    progress = pyqtSignal(int)       # 0-100
    status   = pyqtSignal(str, str)  # (text, colour-tag)
    done     = pyqtSignal(bool, str) # (success, final_status_text)

class SerialReader(QThread):
    def __init__(self, ser):
        super().__init__()
        self.signals = SerialSignals()
        self._ser = ser
        self._stop_event = threading.Event()

    def run(self):
        buf = b""
        while not self._stop_event.is_set():
            try:
                if self._ser.in_waiting:
                    buf += self._ser.read(self._ser.in_waiting)
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        text = line.decode("utf-8", "replace").rstrip("\r").replace("\x00", "")
                        self.signals.line_received.emit(text)
                else:
                    time.sleep(0.02)
            except Exception:
                break

    def stop(self):
        self._stop_event.set()
        self.wait(1000)



# ═══════════════════════════════════════════════════════════════════════════════
# Coloured text widget
# ═══════════════════════════════════════════════════════════════════════════════
class ColourEdit(QTextEdit):

    def __init__(self, parent=None, bg=BG):
        super().__init__(parent)
        self.setReadOnly(True)
        self.setStyleSheet(f"background: {bg}; border: none;")
        mono = QFont(MONO_FONT, 11)
        self.setFont(mono)

    def append_coloured(self, text: str, tag: str = ""):
        colour = TAG_COLORS.get(tag, FG)
        fmt = QTextCharFormat()
        fmt.setForeground(QColor(colour))
        cursor = self.textCursor()
        cursor.movePosition(QTextCursor.MoveOperation.End)
        cursor.insertText(text, fmt)
        self.setTextCursor(cursor)
        self.ensureCursorVisible()


# ═══════════════════════════════════════════════════════════════════════════════
# Main Window
# ═══════════════════════════════════════════════════════════════════════════════
class FemtoClawApp(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"{APP}  v{VER}")
        self.resize(1120, 760)
        self.setMinimumSize(860, 580)

        self._ser: serial.Serial | None = None
        self._reader: SerialReader | None = None
        self._connected = False
        self._connected_port = ""          # track which port is open
        self._flashing = False
        self._compiling = False
        self._cancel_compile = False       # set True to abort compile
        self._compile_proc = None          # subprocess.Popen reference
        self._fw_path = ""
        self._cpp_path = ""
        self._history: list[str] = []
        self._hidx = -1

        # Board config state
        self._board_md_path: str = ""
        self._board_push_thread: threading.Thread | None = None
        self._board_pushing: bool = False
        # Live pin states: {pin_num: 0|1} updated by Sync
        self._board_pin_states: dict[int, int] = {}


        self._cfg = {
            "wifi_ssid": "", "wifi_pass": "",
            "llm_provider": "openrouter", "llm_api_key": "",
            "llm_api_base": "https://openrouter.ai/api/v1",
            "llm_model": "gpt-4o-mini",
            "max_tokens": 1024, "temperature": 0.7,
            "channels": {
                "telegram":  {"enabled": False, "token": "", "allow_from": []},
                "discord":   {"enabled": False, "token": "", "channel_id": "", "allow_from": []}
            }
        }

        # Compile/flash progress signals
        self._csig = CompileSignals()
        self._csig.progress_show.connect(self._set_compile_prog)
        self._csig.progress_pct.connect(self._show_flash_prog)
        self._csig.compile_btn_state.connect(self._set_compile_btn)

        # Board-push progress signals (connected after _build_ui creates the widgets)
        self._bpsig = BoardPushSignals()

        self._build_ui()
        # Wire board-push signals now that the widgets exist
        self._bpsig.progress.connect(self._board_prog.setValue)
        self._bpsig.status.connect(
            lambda txt, tag: (self._board_status_lbl.setText(txt), self._tw(txt, tag)))
        self._bpsig.done.connect(self._on_board_push_done)
        self._refresh_ports()
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._refresh_ports)
        self._timer.start(3000)

    # ── Build top-level UI ────────────────────────────────────────────────────
    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        vbox = QVBoxLayout(central)
        vbox.setContentsMargins(0, 0, 0, 0)
        vbox.setSpacing(0)

        # Banner
        banner = QFrame()
        banner.setObjectName("banner")
        banner.setFixedHeight(68)
        blay = QHBoxLayout(banner)
        blay.setContentsMargins(16, 0, 16, 0)

        lbl_icon = QLabel("🦞")
        lbl_icon.setFont(QFont("Segoe UI Emoji", 22))
        lbl_icon.setStyleSheet(f"color: {PURPLE}; background: transparent;")
        blay.addWidget(lbl_icon)

        lbl_title = QLabel("FemtoClaw MCU Terminal")
        lbl_title.setFont(QFont("Segoe UI", 14, QFont.Weight.Bold))
        lbl_title.setStyleSheet(f"color: {PURPLE}; background: transparent;")
        blay.addWidget(lbl_title)

        lbl_ver = QLabel(f"v{VER}")
        lbl_ver.setFont(QFont("Segoe UI", 9))
        lbl_ver.setStyleSheet(f"color: {FGDIM}; background: transparent; padding-left: 4px;")
        blay.addWidget(lbl_ver)

        blay.addStretch()

        self._stlbl = QLabel("● Disconnected")
        self._stlbl.setFont(QFont("Segoe UI", 10))
        self._stlbl.setStyleSheet(f"color: {RED}; background: transparent;")
        blay.addWidget(self._stlbl)

        vbox.addWidget(banner)

        # Separator
        sep = QFrame()
        sep.setFrameShape(QFrame.Shape.HLine)
        sep.setStyleSheet("background: #30363d;")
        sep.setFixedHeight(1)
        vbox.addWidget(sep)

        # Tabs
        self._tabs = QTabWidget()
        self._tabs.setDocumentMode(True)
        vbox.addWidget(self._tabs)

        self._tabs.addTab(self._mk_flash_tab(),    "  ⚡ Flash  ")
        self._tabs.addTab(self._mk_term_tab(),     "  🖥  Terminal  ")
        self._tabs.addTab(self._mk_llm_tab(),      "  ⚙️  LLM & WiFi  ")
        self._tabs.addTab(self._mk_channels_tab(), "  💬  Channels  ")
        self._tabs.addTab(self._mk_board_tab(),    "  🔌  Control  ")
        self._tabs.addTab(self._mk_about_tab(),    "  ℹ  About  ")

    # ═══════════════════════════════════════════════════════════════════════════
    # TAB 1 — Flash
    # ═══════════════════════════════════════════════════════════════════════════
    def _mk_flash_tab(self) -> QWidget:
        w = QWidget()
        vbox = QVBoxLayout(w)
        vbox.setContentsMargins(12, 10, 12, 10)
        vbox.setSpacing(6)

        # ── Port row ──────────────────────────────────────────────────────────
        bar = QHBoxLayout()
        bar.addWidget(QLabel("Port:"))
        self._port_cb = QComboBox(); self._port_cb.setMinimumWidth(150)
        bar.addWidget(self._port_cb)
        bar.addWidget(QLabel("Baud:"))
        self._baud_cb = QComboBox()
        for b in BAUDS: self._baud_cb.addItem(str(b), b)
        self._baud_cb.setCurrentText(str(BAUD0))
        self._baud_cb.setMinimumWidth(90)
        bar.addWidget(self._baud_cb)
        bar.addWidget(QLabel("Board:"))
        self._board_cb = QComboBox()
        for brd in ["ESP32", "ESP32-S3", "ESP32-C3", "Pico W"]:
            self._board_cb.addItem(brd)
        bar.addWidget(self._board_cb)
        btn_ref = QPushButton("↻ Refresh")
        btn_ref.clicked.connect(self._refresh_ports)
        bar.addWidget(btn_ref)
        self._conn_btn = styled_btn("Connect", "green")
        self._conn_btn.clicked.connect(self._toggle_conn)
        bar.addWidget(self._conn_btn)
        bar.addStretch()
        vbox.addLayout(bar)

        # ── Device list ───────────────────────────────────────────────────────
        dev_grp = QGroupBox("  USB Serial Devices  ")
        dev_lay = QVBoxLayout(dev_grp)
        dev_lay.setContentsMargins(6, 16, 6, 6)
        self._dtree = QTreeWidget()
        self._dtree.setHeaderLabels(["Port", "Description", "Hardware ID"])
        self._dtree.setColumnWidth(0, 110)
        self._dtree.setColumnWidth(1, 340)
        self._dtree.setColumnWidth(2, 320)
        self._dtree.setAlternatingRowColors(True)
        self._dtree.itemClicked.connect(self._dev_sel)
        self._dtree.setMaximumHeight(120)
        dev_lay.addWidget(self._dtree)
        vbox.addWidget(dev_grp)

        # ── Step 1: Compile ───────────────────────────────────────────────────
        cf = QGroupBox("  Step 1 — Compile Source (optional)  ")
        cf_lay = QGridLayout(cf)
        cf_lay.setContentsMargins(8, 18, 8, 8)
        cf_lay.setColumnStretch(1, 1)
        cf_lay.addWidget(QLabel("Firmware source:"), 0, 0)
        self._cpp_edit = QLineEdit(); self._cpp_edit.setReadOnly(True)
        cf_lay.addWidget(self._cpp_edit, 0, 1)
        btn_cpp = QPushButton("Browse source")
        btn_cpp.clicked.connect(self._browse_cpp)
        cf_lay.addWidget(btn_cpp, 0, 2)
        self._compile_btn = styled_btn("🔨 Compile", "blue")
        self._compile_btn.clicked.connect(self._toggle_compile)
        cf_lay.addWidget(self._compile_btn, 0, 3)
        self._pio_hint = QLabel("")
        self._pio_hint.setStyleSheet(f"color: {FGDIM}; font-size: 8pt;")
        cf_lay.addWidget(self._pio_hint, 1, 0, 1, 4)
        self._update_pio_hint()
        # Progress bar + status label
        self._compile_prog = QProgressBar()
        self._compile_prog.setRange(0, 0)
        self._compile_prog.setFixedHeight(6)
        self._compile_prog.setVisible(False)
        self._compile_prog.setStyleSheet(
            f"QProgressBar{{ background:{BGINP}; border:1px solid #30363d; border-radius:3px; }}"
            f"QProgressBar::chunk{{ background:{BLUE}; border-radius:3px; }}")
        cf_lay.addWidget(self._compile_prog, 2, 0, 1, 4)
        self._compile_status = QLabel("")
        self._compile_status.setStyleSheet(f"color:{FGDIM}; font-size:8pt;")
        cf_lay.addWidget(self._compile_status, 3, 0, 1, 4)
        vbox.addWidget(cf)

        # ── Step 2: Flash ─────────────────────────────────────────────────────
        ff = QGroupBox("  Step 2 — Flash Firmware  ")
        ff_lay = QGridLayout(ff)
        ff_lay.setContentsMargins(8, 18, 8, 8)
        ff_lay.setColumnStretch(1, 1)
        ff_lay.addWidget(QLabel(".bin / .uf2:"), 0, 0)
        self._fw_edit = QLineEdit(); self._fw_edit.setReadOnly(True)
        ff_lay.addWidget(self._fw_edit, 0, 1)
        btn_brow = QPushButton("Browse")
        btn_brow.clicked.connect(self._browse)
        ff_lay.addWidget(btn_brow, 0, 2)
        btn_flash = styled_btn("⚡ Flash", "blue")
        btn_flash.clicked.connect(self._flash)
        ff_lay.addWidget(btn_flash, 0, 3)
        # Flash progress bar
        self._flash_prog = QProgressBar()
        self._flash_prog.setRange(0, 100)
        self._flash_prog.setValue(0)
        self._flash_prog.setFixedHeight(14)
        self._flash_prog.setVisible(False)
        self._flash_prog.setFormat("%p%")
        self._flash_prog.setStyleSheet(
            f"QProgressBar{{ background:{BGINP}; border:1px solid #30363d; border-radius:4px;"
            f"  color:{FG}; font-size:8pt; text-align:center; }}"
            f"QProgressBar::chunk{{ background:{GREEN}; border-radius:4px; }}")
        ff_lay.addWidget(self._flash_prog, 1, 0, 1, 4)
        self._flash_status = QLabel("")
        self._flash_status.setStyleSheet(f"color:{FGDIM}; font-size:8pt;")
        ff_lay.addWidget(self._flash_status, 2, 0, 1, 4)
        # Keep _prog as alias so existing show/hide calls still work (compile indeterminate)
        self._prog = self._compile_prog
        vbox.addWidget(ff)

        # ── Log ───────────────────────────────────────────────────────────────
        log_grp = QGroupBox("  Compile & Flash Log  ")
        log_lay = QVBoxLayout(log_grp)
        log_lay.setContentsMargins(4, 16, 4, 4)
        self._flog = ColourEdit(bg=BGINP)
        log_lay.addWidget(self._flog)
        vbox.addWidget(log_grp, 1)

        self._flog_w(f"FemtoClaw Flasher v{VER} ready.\n"
                     "Step 1: Browse a firmware source → Compile (needs: pip install platformio)\n"
                     "Step 2: Browse a .bin/.uf2   → Flash  (needs: pip install esptool for ESP32)\n",
                     "info")
        return w

    # ═══════════════════════════════════════════════════════════════════════════
    # TAB 2 — Terminal
    # ═══════════════════════════════════════════════════════════════════════════
    def _mk_term_tab(self) -> QWidget:
        w = QWidget()
        vbox = QVBoxLayout(w)
        vbox.setContentsMargins(4, 4, 4, 4)
        vbox.setSpacing(4)

        self._term = ColourEdit()
        vbox.addWidget(self._term, 1)

        # Input row
        ir = QFrame()
        ir.setStyleSheet(f"background: {BGPAN};")
        ir_lay = QHBoxLayout(ir)
        ir_lay.setContentsMargins(6, 4, 6, 4)
        prompt = QLabel("›")
        prompt.setFont(QFont(MONO_FONT, 14, QFont.Weight.Bold))
        prompt.setStyleSheet(f"color: {GREEN}; background: transparent;")
        ir_lay.addWidget(prompt)
        self._ent = QLineEdit()
        self._ent.setStyleSheet(f"background: {BGINP}; border: none; font-family: {MONO_FONT}; font-size: 11pt;")
        self._ent.returnPressed.connect(self._send)
        ir_lay.addWidget(self._ent, 1)
        btn_send = styled_btn("Send", "green")
        btn_send.clicked.connect(self._send)
        ir_lay.addWidget(btn_send)
        btn_clr = QPushButton("Clear")
        btn_clr.clicked.connect(self._clear_term)
        ir_lay.addWidget(btn_clr)
        vbox.addWidget(ir)

        # Key bindings
        self._ent.installEventFilter(self)

        self._tw(WELCOME, "banner")
        self._tw("Connect to a board via the Flash tab first.\n", "dim")
        return w

    def eventFilter(self, obj, event):
        from PyQt6.QtCore import QEvent
        from PyQt6.QtGui import QKeyEvent
        if obj is self._ent and event.type() == QEvent.Type.KeyPress:
            key = event.key()
            if key == Qt.Key.Key_Up:
                self._hup(); return True
            if key == Qt.Key.Key_Down:
                self._hdown(); return True
            if key == Qt.Key.Key_Tab:
                self._tab_complete(); return True
        return super().eventFilter(obj, event)

    # ═══════════════════════════════════════════════════════════════════════════
    # TAB 3 — LLM & WiFi
    # ═══════════════════════════════════════════════════════════════════════════
    def _mk_llm_tab(self) -> QWidget:
        w = QWidget()
        outer = QVBoxLayout(w)
        outer.setContentsMargins(12, 10, 12, 10)

        top_row = QHBoxLayout()

        # WiFi group
        wf = QGroupBox("  WiFi  ")
        wf_form = QFormLayout(wf)
        wf_form.setContentsMargins(10, 18, 10, 10)
        self._v_ssid  = QLineEdit(self._cfg["wifi_ssid"])
        self._v_wpass = QLineEdit(self._cfg["wifi_pass"])
        self._v_wpass.setEchoMode(QLineEdit.EchoMode.Password)
        wf_form.addRow("SSID:", self._v_ssid)
        wf_form.addRow("Password:", self._v_wpass)
        top_row.addWidget(wf)

        # LLM group
        lf = QGroupBox("  LLM Provider  ")
        lf_form = QFormLayout(lf)
        lf_form.setContentsMargins(10, 18, 10, 10)
        self._v_prov = QComboBox()
        for p in ["openrouter", "openai", "anthropic", "deepseek", "groq", "zhipu", "ollama"]:
            self._v_prov.addItem(p)
        self._v_prov.setCurrentText(self._cfg["llm_provider"])
        self._v_prov.currentTextChanged.connect(self._prov_sel)
        lf_form.addRow("Provider:", self._v_prov)
        self._v_akey = QLineEdit(self._cfg["llm_api_key"])
        self._v_akey.setEchoMode(QLineEdit.EchoMode.Password)
        lf_form.addRow("API Key:", self._v_akey)
        self._v_abase = QLineEdit(self._cfg["llm_api_base"])
        lf_form.addRow("API Base URL:", self._v_abase)
        self._v_model = QLineEdit(self._cfg["llm_model"])
        lf_form.addRow("Model:", self._v_model)
        top_row.addWidget(lf)

        outer.addLayout(top_row)

        bot_row = QHBoxLayout()

        # Agent settings
        af = QGroupBox("  Agent Settings  ")
        af_form = QFormLayout(af)
        af_form.setContentsMargins(10, 18, 10, 10)
        self._v_maxtok = QSpinBox()
        self._v_maxtok.setRange(128, 8192); self._v_maxtok.setSingleStep(128)
        self._v_maxtok.setValue(self._cfg["max_tokens"])
        af_form.addRow("Max Tokens:", self._v_maxtok)
        temp_row = QHBoxLayout()
        self._v_temp = QSlider(Qt.Orientation.Horizontal)
        self._v_temp.setRange(0, 200); self._v_temp.setValue(int(self._cfg["temperature"] * 100))
        self._v_temp.setFixedWidth(180)
        self._temp_lbl = QLabel(f"{self._cfg['temperature']:.2f}")
        self._temp_lbl.setStyleSheet(f"color: {BLUE}; min-width: 36px;")
        self._v_temp.valueChanged.connect(lambda v: self._temp_lbl.setText(f"{v/100:.2f}"))
        temp_row.addWidget(self._v_temp)
        temp_row.addWidget(self._temp_lbl)
        temp_row.addStretch()
        temp_widget = QWidget(); temp_widget.setLayout(temp_row)
        af_form.addRow("Temperature:", temp_widget)
        bot_row.addWidget(af)

        # Quick presets
        qf = QGroupBox("  Quick Model Presets  ")
        qf_grid = QGridLayout(qf)
        qf_grid.setContentsMargins(10, 18, 10, 10)
        presets = [
            ("GPT-4o Mini",    "openai",      "gpt-4o-mini",                "https://api.openai.com/v1"),
            ("GPT-4o",         "openai",      "gpt-4o",                     "https://api.openai.com/v1"),
            ("Deepseek V3",    "openrouter",  "deepseek/deepseek-chat",     "https://openrouter.ai/api/v1"),
            ("Llama 3.1 70B",  "groq",        "llama-3.1-70b-versatile",    "https://api.groq.com/openai/v1"),
            ("Gemma 2 9B",     "groq",        "gemma2-9b-it",               "https://api.groq.com/openai/v1"),
            ("GLM-4.7",        "zhipu",       "glm-4.7",                    "https://open.bigmodel.cn/api/paas/v4"),
            ("Ollama local",   "ollama",      "llama3",                     "http://localhost:11434/v1"),
        ]
        for i, (lbl, prov, model, base) in enumerate(presets):
            btn = QPushButton(lbl)
            def _ap(checked=False, pv=prov, mv=model, bv=base):
                self._v_prov.setCurrentText(pv)
                self._v_model.setText(mv)
                self._v_abase.setText(bv)
            btn.clicked.connect(_ap)
            qf_grid.addWidget(btn, i // 4, i % 4)
        bot_row.addWidget(qf)

        outer.addLayout(bot_row)

        # Buttons
        br = QHBoxLayout()
        br.addStretch()
        btn_save = styled_btn("💾 Save Config", "green")
        btn_save.clicked.connect(self._save_cfg)
        br.addWidget(btn_save)
        btn_push = styled_btn("📤 Send to Board", "blue")
        btn_push.clicked.connect(self._push_cfg)
        br.addWidget(btn_push)
        br.addStretch()
        outer.addLayout(br)
        outer.addStretch()
        return w

    # ═══════════════════════════════════════════════════════════════════════════
    # TAB 4 — Channels
    # ═══════════════════════════════════════════════════════════════════════════
    def _mk_channels_tab(self) -> QWidget:
        w = QWidget()
        outer = QVBoxLayout(w)
        outer.setContentsMargins(12, 10, 12, 10)

        row = QHBoxLayout()

        # ── Telegram ──────────────────────────────────────────────────────────
        tgf = QGroupBox("  📱  Telegram  ")
        tg_lay = QGridLayout(tgf)
        tg_lay.setContentsMargins(10, 18, 10, 10)
        tg_lay.setColumnStretch(1, 1)

        self._tg_en = QCheckBox("Enable Telegram channel")
        tg_lay.addWidget(self._tg_en, 0, 0, 1, 2)

        tg_lay.addWidget(QLabel("Bot Token:"), 1, 0)
        self._tg_token = QLineEdit(); self._tg_token.setEchoMode(QLineEdit.EchoMode.Password)
        tg_lay.addWidget(self._tg_token, 1, 1)

        hint = QLabel("Get token from @BotFather · Get user ID from @userinfobot")
        hint.setStyleSheet(f"color: {FGDIM}; font-size: 8pt;")
        tg_lay.addWidget(hint, 2, 0, 1, 2)

        tg_lay.addWidget(QLabel("Allow From (user IDs):"), 3, 0, 1, 2)
        note = QLabel("Leave empty to allow everyone.")
        note.setStyleSheet(f"color: {FGDIM}; font-size: 8pt;")
        tg_lay.addWidget(note, 4, 0, 1, 2)

        self._tg_allow_list = QListWidget(); self._tg_allow_list.setMaximumHeight(80)
        tg_lay.addWidget(self._tg_allow_list, 5, 0, 1, 2)

        self._tg_add_var = QLineEdit(); self._tg_add_var.setPlaceholderText("User ID")
        tg_lay.addWidget(self._tg_add_var, 6, 0)
        btn_tg_add = styled_btn("➕ Add ID", "tg")
        btn_tg_add.clicked.connect(self._tg_add)
        tg_lay.addWidget(btn_tg_add, 6, 1)
        btn_tg_rm = QPushButton("➖ Remove Selected")
        btn_tg_rm.clicked.connect(self._tg_remove)
        tg_lay.addWidget(btn_tg_rm, 7, 0, 1, 2)

        btn_push_tg = styled_btn("📤 Push Telegram Config to Board", "tg")
        btn_push_tg.clicked.connect(self._push_tg)
        tg_lay.addWidget(btn_push_tg, 8, 0, 1, 2)

        row.addWidget(tgf)

        # ── Discord ───────────────────────────────────────────────────────────
        dcf = QGroupBox("  🎮  Discord  ")
        dc_lay = QGridLayout(dcf)
        dc_lay.setContentsMargins(10, 18, 10, 10)
        dc_lay.setColumnStretch(1, 1)

        self._dc_en = QCheckBox("Enable Discord channel")
        dc_lay.addWidget(self._dc_en, 0, 0, 1, 2)

        dc_lay.addWidget(QLabel("Bot Token:"), 1, 0)
        self._dc_token = QLineEdit(); self._dc_token.setEchoMode(QLineEdit.EchoMode.Password)
        dc_lay.addWidget(self._dc_token, 1, 1)

        dc_lay.addWidget(QLabel("Channel ID:"), 2, 0)
        self._dc_chan = QLineEdit()
        dc_lay.addWidget(self._dc_chan, 2, 1)

        dchint = QLabel("Enable 'Message Content Intent' in Discord Developer Portal.")
        dchint.setStyleSheet(f"color: {FGDIM}; font-size: 8pt;")
        dc_lay.addWidget(dchint, 3, 0, 1, 2)

        dc_lay.addWidget(QLabel("Allow From (user IDs):"), 4, 0, 1, 2)
        dcnote = QLabel("Leave empty to allow everyone.")
        dcnote.setStyleSheet(f"color: {FGDIM}; font-size: 8pt;")
        dc_lay.addWidget(dcnote, 5, 0, 1, 2)

        self._dc_allow_list = QListWidget(); self._dc_allow_list.setMaximumHeight(80)
        dc_lay.addWidget(self._dc_allow_list, 6, 0, 1, 2)

        self._dc_add_var = QLineEdit(); self._dc_add_var.setPlaceholderText("User ID")
        dc_lay.addWidget(self._dc_add_var, 7, 0)
        btn_dc_add = styled_btn("➕ Add ID", "dc")
        btn_dc_add.clicked.connect(self._dc_add)
        dc_lay.addWidget(btn_dc_add, 7, 1)
        btn_dc_rm = QPushButton("➖ Remove Selected")
        btn_dc_rm.clicked.connect(self._dc_remove)
        dc_lay.addWidget(btn_dc_rm, 8, 0, 1, 2)

        btn_push_dc = styled_btn("📤 Push Discord Config to Board", "dc")
        btn_push_dc.clicked.connect(self._push_dc)
        dc_lay.addWidget(btn_push_dc, 9, 0, 1, 2)

        row.addWidget(dcf)
        outer.addLayout(row)

        # Combined buttons
        br = QHBoxLayout()
        br.addStretch()
        btn_save_all = styled_btn("💾 Save All Channel Config", "green")
        btn_save_all.clicked.connect(self._save_channels)
        br.addWidget(btn_save_all)
        btn_push_all = styled_btn("📤 Push All Channels to Board", "blue")
        btn_push_all.clicked.connect(self._push_all_channels)
        br.addWidget(btn_push_all)
        br.addStretch()
        outer.addLayout(br)

        hint_txt = (
            'Board shell equivalent:\n'
            '  tg token <TOKEN>    tg allow <USER_ID>    tg enable\n'
            '  dc token <TOKEN>    dc channel <CHAN_ID>  dc enable\n'
            'Or use "set tg_token <TOKEN>" and "set dc_token <TOKEN>"'
        )
        lbl_hint = QLabel(hint_txt)
        lbl_hint.setStyleSheet(f"color: {FGDIM}; font-size: 10pt;")
        outer.addWidget(lbl_hint)
        outer.addStretch()
        return w


    # ═══════════════════════════════════════════════════════════════════════════
    # TAB 5 — Control
    # ═══════════════════════════════════════════════════════════════════════════
    def _mk_board_tab(self) -> QWidget:
        w = QWidget()
        outer = QVBoxLayout(w)
        outer.setContentsMargins(10, 8, 10, 8)
        outer.setSpacing(6)

        # ── Top toolbar ───────────────────────────────────────────────────────
        tb = QHBoxLayout()
        tb.setSpacing(6)

        btn_load = QPushButton("📂 Load .md file")
        btn_load.clicked.connect(self._board_load_file)
        tb.addWidget(btn_load)

        btn_save = QPushButton("💾 Save")
        btn_save.clicked.connect(self._board_save_file)
        tb.addWidget(btn_save)

        tb.addSpacing(10)

        self._board_push_btn = styled_btn("📤 Push to Board", "blue")
        self._board_push_btn.clicked.connect(self._board_push)
        tb.addWidget(self._board_push_btn)

        btn_show = styled_btn("🔍 Board Show", "green")
        btn_show.clicked.connect(lambda: self._send_cmds(["board show"]))
        tb.addWidget(btn_show)

        btn_reset = styled_btn("🗑 Board Reset", "red")
        btn_reset.clicked.connect(self._board_reset)
        tb.addWidget(btn_reset)

        tb.addSpacing(10)

        btn_sync = styled_btn("🔄 Sync Pins", "orange")
        btn_sync.setToolTip("Read live GPIO states from board")
        btn_sync.clicked.connect(self._board_sync)
        tb.addWidget(btn_sync)

        tb.addStretch()

        self._board_status_lbl = QLabel("")
        self._board_status_lbl.setStyleSheet(f"color: {FGDIM}; font-size: 9pt;")
        tb.addWidget(self._board_status_lbl)

        outer.addLayout(tb)

        # ── Push progress bar (hidden until push starts) ──────────────────────
        self._board_prog = QProgressBar()
        self._board_prog.setRange(0, 100)
        self._board_prog.setFixedHeight(6)
        self._board_prog.setVisible(False)
        self._board_prog.setStyleSheet(
            f"QProgressBar{{ background:{BGINP}; border:1px solid #30363d; border-radius:3px; }}"
            f"QProgressBar::chunk{{ background:{GREEN}; border-radius:3px; }}")
        outer.addWidget(self._board_prog)

        # ── Main splitter: editor (top) / pin preview (bottom) ────────────────
        splitter = QSplitter(Qt.Orientation.Vertical)

        # ── Markdown editor ───────────────────────────────────────────────────
        ed_frame = QGroupBox("  CONTROL.md  ")
        ed_lay = QVBoxLayout(ed_frame)
        ed_lay.setContentsMargins(6, 14, 6, 6)

        self._board_editor = QPlainTextEdit()
        self._board_editor.setFont(QFont(MONO_FONT, 10))
        self._board_editor.setStyleSheet(
            f"background: {BGINP}; color: {FG}; border: none; "
            f"font-family: \"{MONO_FONT}\", monospace;")
        self._board_editor.setPlaceholderText(
            "# My Board\n"
            "Board: [Board_Name]\n\n"
            "## GPIO Pins\n\n"
            "| Pin | Mode         | Name    | Logic    | Description                   |\n"
            "|-----|--------------|---------|----------|-------------------------------|\n"
            "| 2   | OUTPUT       | led     |  normal  | Onboard LED, High=on, LOW=off |\n"
            "| 9   | INPUT_PULLUP | btn     |  normal  | Boot button LOW=pressed       |\n"
        )
        self._board_editor.textChanged.connect(self._board_editor_changed)
        ed_lay.addWidget(self._board_editor)

        splitter.addWidget(ed_frame)

        # ── Pin preview table ─────────────────────────────────────────────────
        pv_frame = QGroupBox("  Pin Preview  ")
        pv_lay = QVBoxLayout(pv_frame)
        pv_lay.setContentsMargins(6, 14, 6, 6)

        pv_hdr = QHBoxLayout()
        pv_lbl = QLabel("Parsed from editor: click 🔄 Sync to read live states from board")
        pv_lbl.setStyleSheet(f"color: {FGDIM}; font-size: 10pt;")
        pv_hdr.addWidget(pv_lbl)
        pv_hdr.addStretch()
        btn_preview = QPushButton("⟳ Refresh Preview")
        btn_preview.setFixedHeight(24)
        btn_preview.setStyleSheet(f"font-size: 8pt; padding: 2px 8px;")
        btn_preview.clicked.connect(self._board_refresh_preview)
        pv_hdr.addWidget(btn_preview)
        pv_lay.addLayout(pv_hdr)

        self._board_tree = QTreeWidget()
        self._board_tree.setHeaderLabels(
            ["●", "Type", "Pin", "Mode", "Logic", "Name", "Description", "State"])
        self._board_tree.setColumnWidth(0, 20)
        self._board_tree.setColumnWidth(1, 50)
        self._board_tree.setColumnWidth(2, 40)
        self._board_tree.setColumnWidth(3, 110)
        self._board_tree.setColumnWidth(4, 70)
        self._board_tree.setColumnWidth(5, 110)
        self._board_tree.setColumnWidth(6, 220)
        self._board_tree.setColumnWidth(7, 70)
        self._board_tree.setAlternatingRowColors(True)
        self._board_tree.setMaximumHeight(200)
        pv_lay.addWidget(self._board_tree)

        splitter.addWidget(pv_frame)
        splitter.setSizes([420, 220])
        outer.addWidget(splitter, 1)

        # ── Hint bar ──────────────────────────────────────────────────────────
        hint = QLabel(
            "💡  Write [CONTROL].md in the editor  →  📤 Push to Board  →  Board parses & configures hardware  →  AI uses pin names automatically"
        )
        hint.setStyleSheet(f"color: {FGDIM}; font-size: 9pt; padding: 2px 0;")
        hint.setWordWrap(True)
        outer.addWidget(hint)

        return w

    # ── Board tab helpers ──────────────────────────────────────────────────────

    def _board_editor_changed(self):
        """Live-preview as user types — refresh the pin table."""
        self._board_refresh_preview()

    def _board_load_file(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "Load CONTROL.md", self._board_md_path or "",
            "Markdown (*.md);;All Files (*.*)")
        if path:
            try:
                with open(path, "r", encoding="utf-8") as f:
                    content = f.read()
                self._board_editor.setPlainText(content)
                self._board_md_path = path
                self._board_status_lbl.setText(f"Loaded: {os.path.basename(path)}")
            except Exception as e:
                QMessageBox.critical(self, "Load failed", str(e))

    def _board_save_file(self):
        path = self._board_md_path
        if not path:
            path, _ = QFileDialog.getSaveFileName(
                self, "Save CONTROL.md", "CONTROL.md",
                "Markdown (*.md);;All Files (*.*)")
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(self._board_editor.toPlainText())
            self._board_md_path = path
            self._board_status_lbl.setText(f"Saved: {os.path.basename(path)}")
        except Exception as e:
            QMessageBox.critical(self, "Save failed", str(e))

    def _on_board_push_done(self, success: bool, msg: str):
        """Main-thread slot: tidy up after a board push (success or failure)."""
        self._board_push_btn.setEnabled(True)
        self._board_push_btn.setText("📤 Push to Board")
        self._board_status_lbl.setText(msg)
        if success:
            self._board_prog.setValue(100)
        QTimer.singleShot(2000, lambda: self._board_prog.setVisible(False))

    def _board_push(self):
        if not self._connected or not self._ser:
            QMessageBox.warning(self, "Not connected",
                "Connect to the board first (Flash tab).")
            return
        if self._board_pushing:
            return
        md = self._board_editor.toPlainText().strip()
        if not md:
            QMessageBox.warning(self, "Empty", "Write a [CONTROl].md first.")
            return
        self._board_push_btn.setEnabled(False)
        self._board_push_btn.setText("Pushing…")
        self._board_prog.setValue(0)
        self._board_prog.setVisible(True)
        self._board_status_lbl.setText("Pushing…")
        self._board_pushing = True

        sig = self._bpsig   # capture before entering thread

        def _run():
            try:
                b64 = base64.b64encode(md.encode("utf-8")).decode()
                total_chunks = (len(b64) + 199) // 200
                self._ser.write(b"board push begin\r\n")
                time.sleep(0.08)
                for ci, i in enumerate(range(0, len(b64), 200)):
                    chunk = b64[i:i+200]
                    self._ser.write(f"board push chunk {chunk}\r\n".encode())
                    time.sleep(0.05)
                    pct = int((ci + 1) / total_chunks * 90)
                    sig.progress.emit(pct)                        # ← signal, not QTimer
                time.sleep(0.05)
                self._ser.write(b"board push end\r\n")
                time.sleep(0.15)
                ok_msg = f"✓ Pushed {len(md)} bytes ({total_chunks} chunks)"
                sig.status.emit(
                    f"[Board] CONTROL.md pushed ({len(md)} chars, {total_chunks} chunks)\n",
                    "board")
                sig.done.emit(True, ok_msg)
            except Exception as e:
                sig.status.emit(f"[Board] Push failed: {e}\n", "err")
                sig.done.emit(False, f"✗ Push failed: {e}")
            finally:
                self._board_pushing = False

        self._board_push_thread = threading.Thread(target=_run, daemon=True)
        self._board_push_thread.start()

    def _board_reset(self):
        if QMessageBox.question(
            self, "Board Reset",
            "This will drive ALL output pins LOW and clear the stored [CONTROL].md.\n\nContinue?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No
        ) == QMessageBox.StandardButton.Yes:
            self._send_cmds(["board reset"])
            self._board_pin_states.clear()
            self._board_refresh_preview()

    def _board_sync(self):
        """Send gpio get <pin> for every OUTPUT pin, read states from terminal."""
        if not self._connected or not self._ser:
            QMessageBox.warning(self, "Not connected", "Connect to board first.")
            return
        pins = self._board_parse_gpio_pins()
        if not pins:
            QMessageBox.information(self, "No pins", "No GPIO pins found in the editor.")
            return
        cmds = []
        for pin_num, mode, name, _ in pins:
            cmds.append(f"gpio get {pin_num}")
        self._send_cmds(cmds)
        self._board_status_lbl.setText("Sync sent — check Terminal for live values")

    # ── Control markdown parser (local, for preview only) ───────────────────────

    def _board_parse_gpio_pins(self) -> list[tuple]:
        """
        Parse ## GPIO Pins table from the editor.
        Returns list of (pin_num, mode, name,logic, desc).
        """
        md = self._board_editor.toPlainText()
        pins = []
        in_gpio = False
        header_done = False
        sep_done = False
        for line in md.splitlines():
            stripped = line.strip()
            if stripped.startswith("## GPIO") or stripped.startswith("## GPIO Pins"):
                in_gpio = True; header_done = False; sep_done = False; continue
            if stripped.startswith("## ") and in_gpio:
                in_gpio = False; continue
            if not in_gpio or not stripped.startswith("|"):
                continue
            # Detect separator row
            inner = stripped.strip("|").strip()
            if all(c in "-|: " for c in inner):
                sep_done = True; continue
            if not header_done:
                header_done = True; continue
            if not sep_done:
                continue
            cells = [c.strip() for c in stripped.strip("|").split("|")]
            if len(cells) >= 3:
                try:
                    pin_num = int(cells[0])
                    mode = cells[1].strip()
                    name = cells[2].strip()
                    logic = cells[3].strip() if len(cells) > 3 else ""  # ← NEW
                    desc = cells[4].strip() if len(cells) > 4 else ""  # ← shifted
                    pins.append((pin_num, mode, name, logic, desc))  # ← add logic
                except ValueError:
                    pass
        return pins

    def _board_parse_serial_ports(self) -> list[tuple]:
        """
        Parse ## Serial Ports table from editor.
        Returns list of (port, baud, rx, tx, name, desc).
        """
        md = self._board_editor.toPlainText()
        serials = []
        in_sec = False; header_done = False; sep_done = False
        for line in md.splitlines():
            s = line.strip()
            if s.startswith("## Serial"):
                in_sec = True; header_done = False; sep_done = False; continue
            if s.startswith("## ") and in_sec:
                in_sec = False; continue
            if not in_sec or not s.startswith("|"): continue
            inner = s.strip("|").strip()
            if all(c in "-|: " for c in inner):
                sep_done = True; continue
            if not header_done:
                header_done = True; continue
            if not sep_done: continue
            cells = [c.strip() for c in s.strip("|").split("|")]
            if len(cells) >= 5:
                try:
                    serials.append((cells[0], int(cells[1]), int(cells[2]),
                                    int(cells[3]), cells[4],
                                    cells[5] if len(cells) > 5 else ""))
                except ValueError:
                    pass
        return serials

    def _board_parse_adc_pins(self) -> list[tuple]:
        """
        Parse ## ADC Pins table from editor.
        Returns list of (pin_num, name, desc).
        """
        md = self._board_editor.toPlainText()
        adcs = []
        in_sec = False; header_done = False; sep_done = False
        for line in md.splitlines():
            s = line.strip()
            if s.startswith("## ADC"):
                in_sec = True; header_done = False; sep_done = False; continue
            if s.startswith("## ") and in_sec:
                in_sec = False; continue
            if not in_sec or not s.startswith("|"): continue
            inner = s.strip("|").strip()
            if all(c in "-|: " for c in inner):
                sep_done = True; continue
            if not header_done:
                header_done = True; continue
            if not sep_done: continue
            cells = [c.strip() for c in s.strip("|").split("|")]
            if len(cells) >= 2:
                try:
                    adcs.append((int(cells[0]), cells[1],
                                 cells[2] if len(cells) > 2 else ""))
                except ValueError:
                    pass
        return adcs

    def _board_refresh_preview(self):
        """Rebuild the pin preview tree from the editor's current content."""
        self._board_tree.clear()
        gpio_pins  = self._board_parse_gpio_pins()
        serial_ports = self._board_parse_serial_ports()
        adc_pins   = self._board_parse_adc_pins()

        MODE_COLOUR = {
            "OUTPUT":         GREEN,
            "INPUT":          BLUE,
            "INPUT_PULLUP":   YELLOW,
            "INPUT_PULLDOWN": ORANGE,
        }

        for pin_num, mode, name, logic, desc in gpio_pins:
            state = self._board_pin_states.get(pin_num, None)
            dot_color = GREEN if state == 1 else (FGDIM if state is None else RED)
            state_txt = ("HIGH" if state == 1 else "LOW" if state == 0 else "—")
            logic_txt = "inverted" if logic.lower() == "inverted" else "normal"
            item = QTreeWidgetItem(["●", "GPIO", str(pin_num), mode, logic_txt, name, desc, state_txt])
            item.setForeground(0, QColor(dot_color))
            item.setForeground(3, QColor(MODE_COLOUR.get(mode, FG)))
            item.setForeground(4, QColor(ORANGE if logic_txt == "inverted" else FGDIM))  # ← highlight inverted
            item.setForeground(7, QColor(GREEN if state == 1 else
                                         (RED if state == 0 else FGDIM)))
            self._board_tree.addTopLevelItem(item)

        for port, baud, rx, tx, name, desc in serial_ports:
            item = QTreeWidgetItem(["●", "UART", port, f"{baud} baud",
                                    name, desc, f"RX={rx} TX={tx}"])
            item.setForeground(0, QColor(PURPLE))
            item.setForeground(1, QColor(PURPLE))
            self._board_tree.addTopLevelItem(item)

        for pin_num, name, desc in adc_pins:
            item = QTreeWidgetItem(["●", "ADC", str(pin_num), "ANALOG",
                                    name, desc, "—"])
            item.setForeground(0, QColor(ORANGE))
            item.setForeground(3, QColor(ORANGE))
            self._board_tree.addTopLevelItem(item)

        total = len(gpio_pins) + len(serial_ports) + len(adc_pins)
        if total == 0:
            self._board_status_lbl.setText("No pins found: Check ## GPIO Pins / ## ADC Pins / ## Serial Ports sections")
        else:
            self._board_status_lbl.setText(
                f"{len(gpio_pins)} GPIO  ·  {len(serial_ports)} UART  ·  {len(adc_pins)} ADC")

    # ═══════════════════════════════════════════════════════════════════════════
    # TAB 6 — About
    # ═══════════════════════════════════════════════════════════════════════════
    def _mk_about_tab(self) -> QWidget:
        w = QWidget()
        vbox = QVBoxLayout(w)
        vbox.setAlignment(Qt.AlignmentFlag.AlignCenter)

        icon = QLabel("🦞")
        icon.setFont(QFont("Segoe UI Emoji", 48))
        icon.setAlignment(Qt.AlignmentFlag.AlignCenter)
        icon.setStyleSheet(f"color: {PURPLE};")
        vbox.addWidget(icon)

        title = QLabel("FemtoClaw MCU Terminal")
        title.setFont(QFont("Segoe UI", 16, QFont.Weight.Bold))
        title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        title.setStyleSheet(f"color: {PURPLE};")
        vbox.addWidget(title)

        ver = QLabel(f"v{VER}  •  Python {sys.version.split()[0]}")
        ver.setAlignment(Qt.AlignmentFlag.AlignCenter)
        ver.setStyleSheet(f"color: {FGDIM};")
        vbox.addWidget(ver)

        about = (
            "MCU port of PicoClaw : flash and interact with MCU boards.\n\n"
            "Channels:  Telegram long-poll  ·  Discord REST polling\n"
            "Flashing:  esptool (ESP32)  ·  picotool / UF2 drag-drop (Pico W)\n"
            "Shell  :   UART CLI —> chat · config · tg/dc · heartbeat\n\n"
            "FemtoClaw firmware uses ~46 KB RAM and ~1 MB flash.\n"
            "Compare: Go PicoClaw runtime uses ~10-20 MB RAM.\n\n"
            "Inspired by: github.com/sipeed/picoclaw  (Apache-2.0)\n\n"
        )
        about_lbl = QLabel(about)
        about_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        vbox.addWidget(about_lbl)

        note = QLabel("NOTE: This project is not affiliated with Sipeed.")
        note.setAlignment(Qt.AlignmentFlag.AlignCenter)
        note.setStyleSheet(f"color: {FGDIM}; font-size: 10pt;")
        vbox.addWidget(note)
        return w

    # ═══════════════════════════════════════════════════════════════════════════
    # Port management
    # ═══════════════════════════════════════════════════════════════════════════
    def _refresh_ports(self):
        if not HAS_SERIAL:
            return
        ports = list(serial.tools.list_ports.comports())

        # ── USB-unplug detection ─────────────────────────────────────────────
        if self._connected and self._connected_port:
            live_devices = [p.device for p in ports]
            if self._connected_port not in live_devices:
                self._disconnect()
                self._tw(f"\n[⚠ Device {self._connected_port} disconnected — USB cable unplugged]\n", "warn")
                self._flog_w(f"⚠ Port {self._connected_port} disappeared — disconnected.\n", "warn")

        # Update combobox
        current = self._port_cb.currentText()
        self._port_cb.blockSignals(True)
        self._port_cb.clear()
        for p in ports:
            self._port_cb.addItem(p.device)
        if current:
            # Try to restore previously selected port
            idx = self._port_cb.findText(current)
            if idx >= 0:
                self._port_cb.setCurrentIndex(idx)
            elif ports:
                # Previously selected port disappeared; fall back to first
                self._port_cb.setCurrentIndex(0)
        elif ports:
            # First launch with no prior selection: auto-select first port
            self._port_cb.setCurrentIndex(0)
        self._port_cb.blockSignals(False)

        # Update the tree
        self._dtree.clear()
        for pt in ports:
            hwid = pt.hwid or ""
            is_usb = any(x in hwid.upper() for x in
                         ["10C4", "1A86", "0403", "CH340", "CP210", "FTDI", "2341"])
            item = QTreeWidgetItem([pt.device, pt.description or "(unknown)", hwid])
            if is_usb:
                for col in range(3):
                    item.setForeground(col, QColor(GREEN))
            self._dtree.addTopLevelItem(item)

    def _dev_sel(self, item: QTreeWidgetItem):
        port = item.text(0)
        idx = self._port_cb.findText(port)
        if idx >= 0:
            self._port_cb.setCurrentIndex(idx)

    # ── Connect / disconnect ──────────────────────────────────────────────────
    def _toggle_conn(self):
        (self._disconnect if self._connected else self._connect)()

    def _connect(self):
        if not HAS_SERIAL:
            QMessageBox.critical(self, "Missing", "pip install pyserial"); return
        port = self._port_cb.currentText()
        if not port:
            QMessageBox.warning(self, "No port", "Select a port."); return
        try:
            baud = int(self._baud_cb.currentText())
            self._ser = serial.Serial(
                port=port, baudrate=baud, timeout=0.1, write_timeout=2,
                dsrdtr=False,      # prevent DTR reset on ESP32-C3 native USB
                rtscts=False,
            )
            self._ser.dtr = False  # explicitly de-assert DTR after open
            self._ser.rts = False  # explicitly de-assert RTS after open
            self._connected = True
            self._connected_port = port
            self._conn_btn.setText("Disconnect")
            self._conn_btn.setStyleSheet(BTN_STYLE["red"])
            self._stlbl.setText(f"● {port}")
            self._stlbl.setStyleSheet(f"color: {GREEN}; background: transparent;")
            self._reader = SerialReader(self._ser)
            self._reader.signals.line_received.connect(self._rx)
            self._reader.start()
            self._tabs.setCurrentIndex(1)
            self._tw(f"\n[Connected to {port} @ {baud} baud]\n", "ok")
            self._flog_w(f"Port {port} open @ {baud} baud.\n", "ok")
        except Exception as e:
            QMessageBox.critical(self, "Connect failed", str(e))

    def _disconnect(self):
        if self._reader:
            self._reader.stop()
            self._reader = None
        if self._ser and self._ser.is_open:
            self._ser.close()
        self._ser = None
        self._connected = False
        self._connected_port = ""
        self._conn_btn.setText("Connect")
        self._conn_btn.setStyleSheet(BTN_STYLE["green"])
        self._stlbl.setText("● Disconnected")
        self._stlbl.setStyleSheet(f"color: {RED}; background: transparent;")
        self._tw("\n[Disconnected]\n", "warn")

    # ── Terminal helpers ──────────────────────────────────────────────────────
    @pyqtSlot(str)
    def _rx(self, line: str):
        c = strip_ansi(line)
        tag = "dim"
        if "[femtoclaw]" in c:        tag = "agent"
        elif "[tool:" in c:           tag = "tool"
        elif "[Telegram]" in c:       tag = "tg"
        elif "[Discord]" in c:        tag = "dc"
        elif "[WiFi]" in c:           tag = "info"
        elif "[heartbeat]" in c:      tag = "warn"
        elif "[Board]" in c:          tag = "board"
        elif "[Action]" in c:         tag = "action"
        elif "[RESULT:" in c:         tag = "result"
        elif "[!" in c or "error" in c.lower(): tag = "err"
        elif "femtoclaw>" in c:       tag = "prompt"
        elif "connected" in c.lower() or "✓" in c: tag = "ok"
        self._tw(c + "\n", tag)

    def _tw(self, text: str, tag: str = ""):
        self._term.append_coloured(text, tag)

    def _clear_term(self):
        self._term.clear()
        self._tw(WELCOME, "banner")

    def _send(self):
        cmd = self._ent.text().strip()
        if not cmd:
            return
        if not self._history or self._history[-1] != cmd:
            self._history.append(cmd)
        self._hidx = len(self._history)
        self._ent.clear()
        if not self._connected or not self._ser:
            self._tw(f"\n[Not connected] {cmd}\n", "err"); return
        self._tw(f"\n$ {cmd}\n", "user")
        try:
            self._ser.write((cmd + "\r\n").encode("utf-8"))
        except Exception as e:
            self._tw(f"[error] {e}\n", "err")

    def _hup(self):
        if self._history and self._hidx > 0:
            self._hidx -= 1
            self._ent.setText(self._history[self._hidx])

    def _hdown(self):
        if self._hidx < len(self._history) - 1:
            self._hidx += 1
            self._ent.setText(self._history[self._hidx])
        else:
            self._ent.clear()

    def _tab_complete(self):
        completions = [
            "help", "status", "connect", "wifi ", "set llm_model ",
            "set llm_api_key ", "set llm_provider ", "set wifi_ssid ",
            "show config", "chat ", "reset session", "reboot",
            "tg token ", "tg allow ", "tg enable", "tg disable",
            "dc token ", "dc channel ", "dc allow ", "dc enable", "dc disable",
        ]
        pre = self._ent.text()
        m = [c for c in completions if c.startswith(pre)]
        if len(m) == 1:
            self._ent.setText(m[0])
        elif m:
            self._tw("\n" + "  ".join(m) + "\n", "dim")

    # ── Flash helpers ─────────────────────────────────────────────────────────
    def _flog_w(self, text: str, tag: str = ""):
        self._flog.append_coloured(text, tag)

    def _browse(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "Select firmware", "",
            "Firmware (*.bin *.uf2);;Binary (*.bin);;UF2 (*.uf2);;All (*.*)")
        if path:
            self._fw_path = path
            self._fw_edit.setText(path)
            self._flog_w(f"Loaded: {path}\n", "info")

    def _browse_cpp(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "Select FemtoClaw source file", "",
            "C++ source (*.cpp *.ino);;All (*.*)")
        if path:
            self._cpp_path = path
            self._cpp_edit.setText(path)
            self._flog_w(f"Source: {path}\n", "info")

    def _update_pio_hint(self):
        pio = find_pio()
        if pio:
            self._pio_hint.setText(f"✓ PlatformIO found: {pio}")
            self._pio_hint.setStyleSheet(f"color: {GREEN}; font-size: 8pt;")
        else:
            self._pio_hint.setText("✗ PlatformIO not found — run:  pip install platformio")
            self._pio_hint.setStyleSheet(f"color: {ORANGE}; font-size: 8pt;")

    def _flash(self):
        if self._flashing:
            return
        fw   = self._fw_path
        port = self._port_cb.currentText()
        board = self._board_cb.currentText()
        if not fw:   QMessageBox.warning(self, "No firmware", "Browse for a .bin/.uf2 first."); return
        if not port: QMessageBox.warning(self, "No port", "Select a serial port."); return
        if self._connected:
            self._disconnect()
        ext = os.path.splitext(fw)[1].lower()
        self._flashing = True
        self._csig.progress_pct.emit(0, "⏳ Preparing to flash…")
        threading.Thread(target=self._do_flash, args=(fw, port, board, ext), daemon=True).start()

    def _do_flash(self, fw, port, board, ext):
        self._log_thread(f"\n──── Flashing {board} on {port} ────\n", "info")
        try:
            if board in ("ESP32", "ESP32-S3", "ESP32-C3"):
                self._flash_esp(fw, port, board, ext)
            elif board == "Pico W":
                self._flash_pico(fw, port, ext)
        except Exception as e:
            self._log_thread(f"[Exception] {e}\n", "err")
        finally:
            self._flashing = False
            self._csig.progress_pct.emit(-1, "")  # -1 = hide signal

    def _log_thread(self, text: str, tag: str = ""):
        """schedule on main thread."""
        QTimer.singleShot(0, lambda t=text, g=tag: self._flog_w(t, g))

    def _show_flash_prog(self, value: int, status: str = ""):
        """Main-thread slot: update flash progress bar. value=-1 means hide."""
        if value < 0:
            self._flash_prog.setVisible(False)
            self._flash_status.setText("")
            return
        self._flash_prog.setVisible(True)
        self._flash_prog.setValue(value)
        colour = GREEN if value >= 100 else ORANGE if value > 0 else FGDIM
        self._flash_status.setStyleSheet(f"color:{colour}; font-size:8pt;")
        self._flash_status.setText(status)

    def _flash_prog_thread(self, value: int, status: str = ""):
        """emit signal to update flash progress bar."""
        self._csig.progress_pct.emit(value, status)

    def _flash_esp(self, fw, port, board, ext):
        ec = find_esptool()
        chips = {"ESP32": "esp32", "ESP32-S3": "esp32s3", "ESP32-C3": "esp32c3"}
        chip = chips.get(board, "esp32")
        baud = "460800" if board == "ESP32-C3" else "921600"

        if ext != ".bin":
            self._log_thread(f"[!] {ext} must be compiled to .bin first.\n", "warn")
            return

        # For ESP32-C3 with ARDUINO_USB_CDC_ON_BOOT=1, PlatformIO produces
        # three separate bin files (no merged binary). Detect them automatically.
        build_dir = os.path.dirname(fw)
        bootloader = os.path.join(build_dir, "bootloader.bin")
        partitions = os.path.join(build_dir, "partitions.bin")
        firmware   = fw  # firmware.bin at 0x10000

        base_cmd = [ec] if ec else [sys.executable, "-m", "esptool"]
        base_cmd += [
            "--chip", chip,
            "--port", port,
            "--baud", baud,
            "--before", "default_reset",
            "--after",  "hard_reset",
            "write_flash",
            "--flash_mode", "dio",
            "--flash_freq", "80m",
            "--flash_size", "detect",
            "-z",
        ]

        if os.path.exists(bootloader) and os.path.exists(partitions):
            # Flash all three parts at their correct addresses
            cmd = base_cmd + [
                "0x0",     bootloader,
                "0x8000",  partitions,
                "0x10000", firmware,
            ]
            self._log_thread(
                "ℹ Detected 3-part build (bootloader + partitions + firmware)\n"
                f"  bootloader → 0x0\n"
                f"  partitions → 0x8000\n"
                f"  firmware   → 0x10000\n", "info")
        else:
            # Fallback: treat as merged binary at 0x0
            cmd = base_cmd + ["0x0", firmware]
            self._log_thread(
                "ℹ Bootloader/partitions not found alongside firmware.\n"
                "  Flashing as merged binary at 0x0.\n", "warn")

        self._log_thread(" ".join(cmd) + "\n", "dim")
        self._run_subprocess(cmd)

    def _flash_pico(self, fw, port, ext):
        if ext == ".uf2":
            pt = shutil.which("picotool")
            if pt:
                self._run_subprocess([pt, "load", fw, "--force-no-reboot", "-F"])
            else:
                self._log_thread(
                    "[!] picotool not found.\n"
                    "    Hold BOOTSEL while plugging in Pico W, then drag .uf2\n"
                    "    onto the RPI-RP2 USB drive.\n", "warn")
        else:
            self._log_thread(f"[!] {ext} must be compiled to .uf2 first.\n", "warn")

    def _run_subprocess(self, cmd):
        # Regex patterns to extract flash progress from esptool output
        # e.g. "Writing at 0x00010000... (5 %)"  or  "100 %"
        pct_re = re.compile(r'\((\d+)\s*%\)|^(\d+)\s*%')
        try:
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, bufsize=1)
            for line in p.stdout:
                line = line.rstrip()
                tag = "dim"
                ll = line.lower()
                if "error" in ll or "failed" in ll:
                    tag = "err"
                elif "warning" in ll:
                    tag = "warn"
                elif "%" in line or "Wrote" in line or "writing" in ll:
                    tag = "ok"
                    # Parse percentage and push to flash progress bar
                    m = pct_re.search(line)
                    if m:
                        pct = int(m.group(1) or m.group(2))
                        self._flash_prog_thread(pct, f"⚡ Flashing… {pct}%")
                elif "hash of data" in ll or "leaving" in ll:
                    tag = "ok"
                    self._flash_prog_thread(100, "✓ Flash complete")
                elif "connecting" in ll or "detecting" in ll or "chip is" in ll:
                    tag = "info"
                    self._flash_prog_thread(0, f"🔌 {line.strip()}")
                self._log_thread(line + "\n", tag)
            p.wait()
            if p.returncode == 0:
                self._log_thread("\n✓ Flash done — board resetting…\n", "ok")
                self._flash_prog_thread(100, "✓ Flash done — board resetting…")
            else:
                self._log_thread(f"\n✗ Flash failed (exit {p.returncode})\n", "err")
                self._flash_prog_thread(0, f"✗ Flash failed (exit {p.returncode})")
        except FileNotFoundError:
            self._log_thread("[!] esptool not found. Run: pip install esptool\n", "err")
            self._flash_prog_thread(0, "✗ esptool not found")

    # ── Compile ───────────────────────────────────────────────────────────────
    def _toggle_compile(self):
        """Compile button handler"""
        if self._compiling:
            # Cancel requested
            self._cancel_compile = True
            if self._compile_proc and self._compile_proc.poll() is None:
                self._compile_proc.kill()
            self._flog_w("\n[Compile cancelled]\n", "warn")
        else:
            self._compile()

    def _set_compile_btn(self, compiling: bool):
        if compiling:
            self._compile_btn.setText("❌ Cancel")
            self._compile_btn.setStyleSheet(BTN_STYLE["red"])
        else:
            self._compile_btn.setText("🔨 Compile")
            self._compile_btn.setStyleSheet(BTN_STYLE["blue"])

    def _compile(self):
        if self._compiling or self._flashing:
            QMessageBox.information(self, "Busy", "Already compiling or flashing."); return
        cpp = self._cpp_path
        if not cpp:
            QMessageBox.warning(self, "No source", "Browse for femtoclaw_mcu.cpp first."); return
        pio = find_pio()
        if not pio:
            QMessageBox.critical(self, "PlatformIO not found",
                                 "Install PlatformIO first:\n\n  pip install platformio\n\n"
                                 "Then restart FemtoClaw Flasher."); return
        board = self._board_cb.currentText()
        env_map = {"ESP32": "esp32", "ESP32-S3": "esp32s3",
                   "ESP32-C3": "esp32c3", "Pico W": "picow"}
        env = env_map.get(board, "esp32")

        # Determine project root:
        file_dir = os.path.dirname(cpp)
        if os.path.basename(file_dir).lower() == "src":
            proj_dir = os.path.dirname(file_dir)
        else:
            proj_dir = file_dir

        # Ensure a src/ subfolder exists and the source file is inside it for PlatformIO handles most reliability.
        src_dir = os.path.join(proj_dir, "src")
        os.makedirs(src_dir, exist_ok=True)
        src_cpp = os.path.join(src_dir, os.path.basename(cpp))
        # Use normcase for case-insensitive comparison on Windows
        if os.path.normcase(os.path.abspath(cpp)) != os.path.normcase(os.path.abspath(src_cpp)):
            import shutil as _shutil
            _shutil.copy2(cpp, src_cpp)
            self._flog_w(f"[auto] Copied source to {src_cpp}\n", "info")

        ini_path = os.path.join(proj_dir, "platformio.ini")
        if not os.path.exists(ini_path):
            self._write_minimal_ini(proj_dir, src_cpp, board)
        threading.Thread(target=self._do_compile, args=(pio, proj_dir, env, board), daemon=True).start()

    def _write_minimal_ini(self, proj_dir: str, cpp_path: str, board: str):
        board_map = {
            "ESP32":    ("espressif32", "esp32dev",            "-DBOARD_ESP32",  "921600"),
            "ESP32-S3": ("espressif32", "esp32-s3-devkitc-1", "-DBOARD_ESP32",  "921600"),
            "ESP32-C3": ("espressif32", "lolin_c3_mini",       "-DBOARD_ESP32",  "460800"),
            "Pico W":   ("raspberrypi", "rpipicow",            "-DBOARD_PICO_W", ""),
        }
        plat, brd, define, upload_speed = board_map.get(board, board_map["ESP32"])
        env_map = {"ESP32": "esp32", "ESP32-S3": "esp32s3", "ESP32-C3": "esp32c3", "Pico W": "picow"}
        env = env_map.get(board, "esp32")
        src_dir_path = os.path.join(proj_dir, "src").replace(chr(92), chr(47))

        if board == "Pico W":
            extra_lines = "board_build.core = earlephilhower\nmonitor_speed = 115200\n"
        elif board == "ESP32-C3":
            # Native USB-CDC: lower baud, disable DTR/RTS toggling so the port
            # does not disappear during flashing, enable release build.
            extra_lines = (
                f"upload_speed = {upload_speed}\n"
                f"monitor_speed = 115200\n"
                f"monitor_dtr = 0\n"
                f"monitor_rts = 0\n"
                f"build_type = release\n"
            )
        else:
            extra_lines = f"upload_speed = {upload_speed}\nmonitor_speed = 115200\n"

        usb_flags = (
            "    -DARDUINO_USB_MODE=1\n"
            "    -DARDUINO_USB_CDC_ON_BOOT=1\n"
            "    -std=gnu++17\n"
        ) if board == "ESP32-C3" else ""

        # For ESP32-C3: unflags gnu++11 (set by the board JSON) and forces gnu++17
        # so C++17 features in femtoclaw_mcu.cpp compile cleanly.
        build_unflags = "build_unflags = -std=gnu++11\n" if board == "ESP32-C3" else ""

        ini = (
            f"; Auto-generated by FemtoClaw for {board}\n"
            f"[platformio]\n"
            f"default_envs = {env}\n"
            f"src_dir = {src_dir_path}\n\n"
            f"[env:{env}]\n"
            f"platform  = {plat}\n"
            f"board     = {brd}\n"
            f"framework = arduino\n"
            f"{build_unflags}"
            f"{extra_lines}"
            f"build_flags =\n"
            f"    -Os\n"
            f"    -ffunction-sections\n"
            f"    -fdata-sections\n"
            f"    -Wl,--gc-sections\n"
            f"    -w\n"
            f"    {define}\n"
            f"    -DCONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=4096\n"
            f"{usb_flags}"
        )
        path = os.path.join(proj_dir, "platformio.ini")
        with open(path, "w") as f:
            f.write(ini)
        self._log_thread(f"[auto] Created platformio.ini in {proj_dir}\n", "info")

    def _set_compile_prog(self, visible: bool, status: str = ""):
        """Main-thread helper: show/hide compile progress bar and set status text."""
        is_error = "x" in status or "failed" in status.lower() or "cancelled" in status.lower()

        if visible and is_error:
            self._compile_prog.setRange(0, 1)
            self._compile_prog.setValue(0)
        elif visible:
            self._compile_prog.setRange(0, 0)
        else:
            self._compile_prog.setRange(0, 0)

        self._compile_prog.setVisible(visible)
        self._compile_status.setText(status)
        self._compile_status.setStyleSheet(
            f"color:{GREEN}; font-size:8pt;" if "✓" in status
            else f"color:{RED}; font-size:8pt;" if "✗" in status or "failed" in status.lower()
            else f"color:{FGDIM}; font-size:8pt;")

    def _ensure_platform(self, pio: str, board: str) -> bool:
        """
        For Pico W: check if the raspberrypi platform is already installed globally.
        If yes, skip straight to compile. If no, install it first (first-time only).
        Returns True on success/already-installed, False on failure.
        """
        if board != "Pico W":
            return True

        # Ask PlatformIO directly => handles partial/corrupt installs
        try:
            result = subprocess.run(
                [pio, "pkg", "list", "--global", "--platform"],
                capture_output=True, text=True
            )
            if "raspberrypi" in result.stdout.lower():
                self._log_thread("[platform] raspberrypi already installed — skipping install.\n", "dim")
                return True
        except Exception:
            pass  # fall through to install attempt if check itself fails

        self._log_thread("[platform] raspberrypi platform not found. Installing (first time only — may take a few minutes)…\n", "info")
        self._csig.progress_show.emit(True, "⏳ Installing Pico W platform…")
        try:
            p = subprocess.Popen(
                [pio, "pkg", "install", "--global", "--platform", "raspberrypi"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1
            )
            for line in p.stdout:
                self._log_thread(line, "dim")
            p.wait()
            if p.returncode == 0:
                self._log_thread("[platform] ✓ raspberrypi platform installed successfully.\n", "ok")
                return True
            else:
                self._log_thread(f"[platform] ✗ Platform install failed (exit {p.returncode})\n", "err")
                self._csig.progress_show.emit(True, "✗ Pico W platform install failed")
                return False
        except Exception as e:
            self._log_thread(f"[platform] ✗ Exception during install: {e}\n", "err")
            self._csig.progress_show.emit(True, f"✗ Platform install error: {e}")
            return False

    def _do_compile(self, pio: str, proj_dir: str, env: str, board: str):
        self._compiling = True
        self._cancel_compile = False
        self._csig.compile_btn_state.emit(True)   # → show "❌ Cancel"
        self._csig.progress_show.emit(True, "⏳ Compiling…")
        self._log_thread(f"\n──── Compiling for {board} (env:{env}) ────\n", "info")
        self._log_thread(f"$ pio run -e {env} --project-dir \"{proj_dir}\"\n", "dim")
        try:
            # ── Ensure Pico W platform is installed before building ───────────
            if not self._ensure_platform(pio, board):
                return
            # ─────────────────────────────────────────────────────────────────
            p = subprocess.Popen(
                [pio, "run", "-e", env, "--project-dir", proj_dir],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1)
            self._compile_proc = p
            for line in p.stdout:
                if self._cancel_compile:
                    p.kill()
                    break
                line = line.rstrip()
                ll = line.lower()
                tag = "dim"
                if "error" in ll:    tag = "err"
                elif "warning" in ll: tag = "warn"
                elif "success" in ll or "bytes" in ll: tag = "ok"
                elif "compiling" in ll or "linking" in ll: tag = "info"
                self._log_thread(line + "\n", tag)
            p.wait()
            if self._cancel_compile:
                self._csig.progress_show.emit(False, "⚠ Compile cancelled")
            elif p.returncode == 0:
                fw = self._find_build_output(proj_dir, env, board)
                if fw:
                    QTimer.singleShot(0, lambda f=fw: self._auto_fill_fw(f))
                    self._log_thread(
                        f"\n✓ Compile done!\n  Output: {fw}\n"
                        f"  → Firmware path auto-filled. Press ⚡ Flash.\n", "ok")
                    self._csig.progress_show.emit(False, "✓ Compile done — firmware ready")
                else:
                    self._log_thread(
                        "\n✓ Compile done but output file not found.\n"
                        "  Check .pio/build/ folder manually.\n", "warn")
                    self._csig.progress_show.emit(False, "⚠ Compile done but .bin not found")
            else:
                self._log_thread(f"\n✗ Compile failed (exit {p.returncode})\n", "err")
                self._csig.progress_show.emit(True, f"✗ Compile failed (exit {p.returncode})")
        except Exception as e:
            self._log_thread(f"[Compile exception] {e}\n", "err")
            self._csig.progress_show.emit(True, f"✗ Exception: {e}")
        finally:
            self._compiling = False
            self._cancel_compile = False
            self._compile_proc = None
            self._csig.compile_btn_state.emit(False)   # → restore "Compile"

    def _auto_fill_fw(self, fw: str):
        self._fw_path = fw
        self._fw_edit.setText(fw)

    def _find_build_output(self, proj_dir: str, env: str, board: str) -> str | None:
        build_dir = os.path.join(proj_dir, ".pio", "build", env)
        if not os.path.isdir(build_dir):
            return None
        ext = ".uf2" if board == "Pico W" else ".bin"
        # Prefer firmware.bin — for ESP32 3-part builds this is the app binary.
        # bootloader.bin and partitions.bin are picked up automatically by _flash_esp.
        for name in ["firmware" + ext, "program" + ext]:
            p = os.path.join(build_dir, name)
            if os.path.exists(p):
                return p
        # Fallback: first .bin that is NOT bootloader or partitions
        skip = {"bootloader.bin", "partitions.bin"}
        for f in sorted(os.listdir(build_dir)):
            if f.endswith(ext) and f not in skip:
                return os.path.join(build_dir, f)
        return None

    # ── Channel helpers ───────────────────────────────────────────────────────
    def _tg_add(self):
        uid = self._tg_add_var.text().strip()
        if uid:
            self._tg_allow_list.addItem(uid)
            self._tg_add_var.clear()

    def _tg_remove(self):
        for item in self._tg_allow_list.selectedItems():
            self._tg_allow_list.takeItem(self._tg_allow_list.row(item))

    def _dc_add(self):
        uid = self._dc_add_var.text().strip()
        if uid:
            self._dc_allow_list.addItem(uid)
            self._dc_add_var.clear()

    def _dc_remove(self):
        for item in self._dc_allow_list.selectedItems():
            self._dc_allow_list.takeItem(self._dc_allow_list.row(item))

    def _tg_allow_items(self) -> list[str]:
        return [self._tg_allow_list.item(i).text()
                for i in range(self._tg_allow_list.count())]

    def _dc_allow_items(self) -> list[str]:
        return [self._dc_allow_list.item(i).text()
                for i in range(self._dc_allow_list.count())]

    def _send_cmds(self, cmds: list[str]):
        if not self._connected or not self._ser:
            QMessageBox.warning(self, "Not connected", "Connect to the board first (Flash tab).")
            return
        self._tabs.setCurrentIndex(1)
        for cmd in cmds:
            self._tw(f"$ {cmd}\n", "user")
            try:
                self._ser.write((cmd + "\r\n").encode("utf-8"))
                time.sleep(0.18)
            except Exception as e:
                self._tw(f"[error] {e}\n", "err"); break

    def _push_tg(self):
        cmds = []
        tok = self._tg_token.text().strip()
        if tok: cmds.append(f"tg token {tok}")
        for uid in self._tg_allow_items():
            cmds.append(f"tg allow {uid}")
        cmds.append("tg enable" if self._tg_en.isChecked() else "tg disable")
        self._send_cmds(cmds)
        self._tw("[Telegram config pushed]\n", "tg")

    def _push_dc(self):
        cmds = []
        tok = self._dc_token.text().strip()
        cid = self._dc_chan.text().strip()
        if tok: cmds.append(f"dc token {tok}")
        if cid: cmds.append(f"dc channel {cid}")
        for uid in self._dc_allow_items():
            cmds.append(f"dc allow {uid}")
        cmds.append("dc enable" if self._dc_en.isChecked() else "dc disable")
        self._send_cmds(cmds)
        self._tw("[Discord config pushed]\n", "dc")

    def _push_all_channels(self):
        self._push_tg(); self._push_dc()

    def _save_channels(self):
        self._cfg["channels"]["telegram"] = {
            "enabled": self._tg_en.isChecked(),
            "token": self._tg_token.text(),
            "allow_from": self._tg_allow_items()
        }
        self._cfg["channels"]["discord"] = {
            "enabled": self._dc_en.isChecked(),
            "token": self._dc_token.text(),
            "channel_id": self._dc_chan.text(),
            "allow_from": self._dc_allow_items()
        }
        QMessageBox.information(self, "Saved",
            "Channel config saved to in-memory config.\n"
            "Use '💾 Save Config' on the LLM tab to write to disk.")

    # ── LLM tab helpers ───────────────────────────────────────────────────────
    def _prov_sel(self, prov: str):
        presets = {
            "openrouter": ("https://openrouter.ai/api/v1",           "openai/gpt-4o-mini"),
            "openai":     ("https://api.openai.com/v1",              "gpt-4o-mini"),
            "anthropic":  ("https://api.anthropic.com/v1",           "claude-haiku-4-5-20251001"),
            "deepseek":   ("https://api.deepseek.com/v1",            "deepseek-chat"),
            "groq":       ("https://api.groq.com/openai/v1",         "llama-3.1-70b-versatile"),
            "zhipu":      ("https://open.bigmodel.cn/api/paas/v4",   "glm-4.7"),
            "ollama":     ("http://localhost:11434/v1",               "llama3"),
        }
        if prov in presets:
            b, m = presets[prov]
            self._v_abase.setText(b)
            self._v_model.setText(m)

    def _collect(self) -> dict:
        return {
            "wifi_ssid":    self._v_ssid.text(),
            "wifi_pass":    self._v_wpass.text(),
            "llm_provider": self._v_prov.currentText(),
            "llm_api_key":  self._v_akey.text(),
            "llm_api_base": self._v_abase.text(),
            "llm_model":    self._v_model.text(),
            "max_tokens":   self._v_maxtok.value(),
            "temperature":  round(self._v_temp.value() / 100, 2),
            "channels": {
                "telegram": {
                    "enabled":    self._tg_en.isChecked(),
                    "token":      self._tg_token.text(),
                    "allow_from": self._tg_allow_items()
                },
                "discord": {
                    "enabled":    self._dc_en.isChecked(),
                    "token":      self._dc_token.text(),
                    "channel_id": self._dc_chan.text(),
                    "allow_from": self._dc_allow_items()
                }
            }
        }

    def _save_cfg(self):
        self._cfg = self._collect()
        path, _ = QFileDialog.getSaveFileName(
            self, "Save FemtoClaw config", "femtoclaw.json",
            "JSON (*.json);;All (*.*)")
        if path:
            with open(path, "w") as f:
                json.dump(self._cfg, f, indent=2)
            QMessageBox.information(self, "Saved", f"Config saved to:\n{path}")

    def _push_cfg(self):
        cfg = self._collect()
        cmds = []
        for key in ("wifi_ssid", "wifi_pass", "llm_provider", "llm_api_key",
                    "llm_api_base", "llm_model"):
            val = cfg.get(key, "")
            if val:
                cmds.append(f"set {key} {val}")
        self._send_cmds(cmds)
        self._tw("\n[LLM/WiFi config pushed]\n", "ok")

    # ── Close ─────────────────────────────────────────────────────────────────
    def closeEvent(self, event):
        self._disconnect()
        event.accept()


# ── Entry point ───────────────────────────────────────────────────────────────
if __name__ == "__main__":
    if not HAS_SERIAL:
        print("pyserial not found! run:  pip install pyserial esptool")
        print("Continuing with limited functionality...\n")
    app = QApplication(sys.argv)
    app.setStyleSheet(STYLE)
    window = FemtoClawApp()
    window.show()
    sys.exit(app.exec())
