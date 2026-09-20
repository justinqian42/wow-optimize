using System;
using System.IO;
using System.Diagnostics;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;
using System.Reflection;
using Microsoft.Win32;

namespace WowOptimizeLauncher {


    // ───────────────────────────────────────────────────────────────
    //  What a switch is for
    // ───────────────────────────────────────────────────────────────
    //
    // A tester asked for the fastest possible configuration and could not tell
    // which switches were making the game faster and which were only counting
    // things. Nothing in the list said. The four kinds below are on every
    // entry now, and MAX PERFORMANCE turns on the ones that help.
    //
    // Keyed by ini key rather than by the label, because labels get reworded
    // and a mis-keyed classification would quietly put a profiler in a
    // performance preset.
    public static class Kinds {
        public const string Perf  = "[+]";   // makes the game faster
        public const string Fix   = "[=]";   // protects or repairs; no speed claim
        public const string Diag  = "[?]";   // measures - costs frames, gives numbers
        public const string Trade = "[-]";   // more frames by changing how it looks
        public const string Log   = "[.]";   // records; negligible cost
        public const string Lost  = "[x]";   // measured and lost, or a known failure
        public const string Unproven = "[!]";   // not proven yet

        // Badged apart from the rest because otherwise they read as speed
        // and the preset below silently disagrees with their own label.
        // Each was believed to help until something measured it.
        private static readonly string[] LostKeys = new string[] {
            "MatrixVectorSse2",    // 3.3 ns a call against the client's 2.5
            "TextureUnloadDelay",  // 0.4% and 0.2% of held textures ever reused
            "UIFrameBatch",        // switches nothing any more
            "LuaGcCoalesce",       // the one tester crash with us truly in the stack
            // Both hold a skeleton and pick what to hold by distance. Reported
            // visibly stuttering on environment animation - lava in Ironforge
            // for the stride, the Deeprun Tram tunnels for the crowd throttle -
            // and neither has ever produced a measured frame gain to weigh
            // against it. Distance is a poor stand-in for whether a held
            // skeleton is seen: a large animation fills the screen at any range.
            "AnimLod",
            "M2AnimStride"
        };

        private static readonly string[] DiagKeys = new string[] {
            "AbTest", "SamplingProfiler", "AddonProfiler", "LuaAddonProfile",
            "LuaAllocCensus", "LuaCompileCensus", "LuaTableCensus", "AnimCensus",
            "DrawCensus", "ShadowStateProbe", "LockSpinHooks", "NoClientPatches",
            "VaCensus", "CameraReplay",
        };
        private static readonly string[] LogKeys = new string[] {
            "SessionLogs", "FlightRecorder", "NetDiag", "CpuTopology",
        };
        private static readonly string[] FixKeys = new string[] {
            "CompatMode", "MemoryPressure", "TimingCvarPin", "CvarNullGuard",
            "PriorityGuard", "DeviceCbGuard", "OomGovernor", "HardwareCursor",
            "MouseClipRelease", "SavedVarsBackup", "MimallocHighArena",
            "RenderNullGuard", "CombatLogLeakFix", "ShadowCascadeHold",
            "LuaGcStockPace", "UIFrameBatch",
        };
        private static readonly string[] TradeKeys = new string[] {
            // Each of these buys frames by changing something the player can see
            // or hear. Frame Rate Limiter Override was in here and does not
            // belong: it replaces the client's own per-frame limiter with a
            // waitable-timer and spin hybrid, which changes when a frame is
            // handed over and nothing about what is in it.
            "QualityGovernor", "MipBiasGovernor", "SpellEffectCulling",
            "SoundVolumeLimit",
        };

        // A replacement for something the client already does, as opposed to a
        // census, a look-and-sound trade, a crash guard or one that lost its
        // measurement. What "TRY THE UNPROVEN ONES" is allowed to turn on.
        public static bool IsReplacement(string key) {
            return !In(DiagKeys, key) && !In(TradeKeys, key) && !In(LostKeys, key)
                && !In(LogKeys, key) && !In(FixKeys, key);
        }

        private static bool In(string[] set, string key) {
            for (int i = 0; i < set.Length; i++) {
                if (set[i] == key) return true;
            }
            return false;
        }

        // Anything not named above speeds the game up. That is what most of
        // this tool is, and listing the exceptions is shorter and stays right
        // as features are added.
        // `unproven` is the SettingItem's Experimental flag. It only changes
        // what a helping switch is called, so a census stays a census and a
        // governor stays a governor either way.
        public static string Of(string key, bool unproven) {
            if (In(LostKeys, key))  return Lost;
            if (In(DiagKeys, key))  return Diag;
            if (In(TradeKeys, key)) return Trade;
            if (In(LogKeys, key))   return Log;
            if (In(FixKeys, key))   return Fix;
            return unproven ? Unproven : Perf;
        }

        // Left off by MAX PERFORMANCE, each for a reason recorded beside it.
        public static readonly string[] NotForSpeed = new string[] {
            // Measures the game. Every one of these costs frames to produce a
            // number, and a player wants the frames.
            "AbTest", "SamplingProfiler", "AddonProfiler", "LuaAddonProfile",
            "LuaAllocCensus", "LuaCompileCensus", "LuaTableCensus", "AnimCensus",
            "DrawCensus", "ShadowStateProbe", "LockSpinHooks", "NoClientPatches",
            "VaCensus", "CameraReplay",

            // Buys frames by making the game look or sound different. That is a
            // real trade and it is the player's to make, not this button's. A
            // tester turned all of these on, saw his view distance pulled from
            // 350 to 262 and back seven times in four minutes, and could only
            // say that something was wrong with the graphics.
            "QualityGovernor", "MipBiasGovernor", "SpellEffectCulling",
            "AnimLod", "M2AnimStride", "SoundVolumeLimit",

            // Keeps a player out of the world. The hook it installs sits in front
            // of InitializeCriticalSection for every module in the process, and a
            // tester bisected his failure to enter the world with ReShade down to
            // this one switch. It is a compatibility hook, not a speed switch, and
            // this button must never turn it on.
            "LockTuningInitHook",

            // Left off because something measured them and the answer was no.
            "CompatMode",          // slower on purpose; it repairs a broken connection
            "MatrixVectorSse2",    // measured against the client: 3.3 ns to its 2.5
            "TextureUnloadDelay",  // two testers: 0.4% and 0.2% of held textures reused
            "UIFrameBatch",        // switches nothing; the two it named have their own
            "LuaGcStockPace",      // turns the GC pacing below it off again
            "LuaGcCoalesce",
        };

        // The order a tab lists them in, and what each run is called. Worst
        // last: a person scrolling a tab meets the reasons to tick something
        // before the reasons not to.
        public static readonly string[] Order = new string[] {
            Perf, Unproven, Fix, Log, Diag, Trade, Lost
        };

        // Short, and each one leads with the reason you would or would not
        // want the run under it. The appearance group used to read MORE FRAMES,
        // DIFFERENT LOOK, which made MAX PERFORMANCE look inconsistent for
        // leaving it off - it reads as performance and it is not; it is a trade
        // against how the game looks, and that is the player's to make.
        public static string Heading(string kind) {
            if (kind == Perf)     return "MAKES IT FASTER";
            if (kind == Unproven) return "NOT PROVEN YET";
            if (kind == Fix)      return "STABILITY AND FIXES";
            if (kind == Log)      return "LOGGING";
            if (kind == Diag)     return "DIAGNOSTICS - COSTS FRAMES";
            if (kind == Trade)    return "CHANGES HOW IT LOOKS OR SOUNDS";
            if (kind == Lost)     return "TRIED, DIDN'T HELP";
            return "";
        }

        public static bool HelpsSpeed(string key) {
            for (int i = 0; i < NotForSpeed.Length; i++) {
                if (NotForSpeed[i] == key) return false;
            }
            return true;
        }
    }

    public class SettingItem {
        public string Section;
        public string Key;
        public bool DefaultVal;
        public CheckBox Ctrl;
        public string Tooltip;

        // Marks a switch that has not been proven. It used to decide which tab the
        // row appeared on, which put nearly half of them on one tab; now it only
        // decides whether the row is marked [+] or [!]. A tester was
        // asked to leave one of these off so we could tell whether it caused their
        // addon errors; they pressed Enable All, it went on with everything else,
        // and the comparison measured nothing. A switch that exists to be left off
        // has to survive the button that turns everything on.
        public bool Experimental;

        public SettingItem(string section, string key, bool defaultVal, CheckBox ctrl, string tooltip)
            : this(section, key, defaultVal, ctrl, tooltip, false) {
        }

        public SettingItem(string section, string key, bool defaultVal, CheckBox ctrl, string tooltip, bool experimental) {
            Section = section;
            Key = key;
            DefaultVal = defaultVal;
            Ctrl = ctrl;
            Tooltip = tooltip;
            Experimental = experimental;
        }
    }

    // ───────────────────────────────────────────────────────────────
    //  Owner-drawn dark-themed CheckBox
    // ───────────────────────────────────────────────────────────────
    public class DarkCheckBox : CheckBox {
        private static readonly Color CyanAccent = Color.FromArgb(0, 229, 255);
        private static readonly Color BorderIdle = Color.FromArgb(100, 110, 140);
        private static readonly Color BoxBg = Color.FromArgb(18, 18, 28);

        public DarkCheckBox() {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer, true);
            ForeColor = Color.White;
            Font = new Font("Segoe UI", 9.75f, FontStyle.Regular);
            Cursor = Cursors.Hand;
            // Twelve pixels of gap under every row meant a forty-two entry
            // tab showed twelve of them and the rest was scrolling.
            Margin = new Padding(5, 3, 5, 5);
            AutoSize = true;
        }

        protected override void OnPaint(PaintEventArgs e) {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.HighQuality;
            g.Clear(Parent != null ? Parent.BackColor : Color.FromArgb(15, 15, 22));

            // Checkbox square (16x16)
            int boxSize = 16;
            int boxY = (Height - boxSize) / 2;
            Rectangle boxRect = new Rectangle(0, boxY, boxSize, boxSize);

            using (SolidBrush bgBrush = new SolidBrush(BoxBg)) {
                g.FillRectangle(bgBrush, boxRect);
            }

            Color borderColor = (Checked || ClientRectangle.Contains(PointToClient(MousePosition))) ? CyanAccent : BorderIdle;
            using (Pen borderPen = new Pen(borderColor, 1.5f)) {
                g.DrawRectangle(borderPen, boxRect);
            }

            // Inner cyan square when checked (8x8 centered)
            if (Checked) {
                int innerSize = 8;
                int innerX = (boxSize - innerSize) / 2;
                int innerY = boxY + (boxSize - innerSize) / 2;
                using (SolidBrush cyanBrush = new SolidBrush(CyanAccent)) {
                    g.FillRectangle(cyanBrush, innerX, innerY, innerSize, innerSize);
                }
            }

            // Text
            using (SolidBrush textBrush = new SolidBrush(ForeColor)) {
                g.DrawString(Text, Font, textBrush, boxSize + 8, (Height - Font.Height) / 2f);
            }
        }

        protected override void OnMouseEnter(EventArgs e) {
            base.OnMouseEnter(e);
            Invalidate();
        }

        protected override void OnMouseLeave(EventArgs e) {
            base.OnMouseLeave(e);
            Invalidate();
        }

        public override Size GetPreferredSize(Size proposedSize) {
            using (Graphics g = CreateGraphics()) {
                SizeF textSize = g.MeasureString(Text, Font);
                return new Size(16 + 8 + (int)Math.Ceiling(textSize.Width) + 4, Math.Max(20, (int)Math.Ceiling(textSize.Height) + 4));
            }
        }
    }

    // ───────────────────────────────────────────────────────────────
    //  Owner-drawn dark-themed Button with hover effect
    // ───────────────────────────────────────────────────────────────
    public class DarkButton : Button {
        private bool _hovering;
        private Color _accentColor;
        private bool _highlight;

        public DarkButton(Color accentColor, bool highlight) {
            _accentColor = accentColor;
            _highlight = highlight;

            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer, true);
            FlatStyle = FlatStyle.Flat;
            FlatAppearance.BorderSize = 0;
            Cursor = Cursors.Hand;
            Font = new Font("Segoe UI", 8.5f, FontStyle.Bold);
            Height = 30;
            Margin = new Padding(0, 0, 0, 6);
        }

        protected override void OnPaint(PaintEventArgs e) {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.HighQuality;

            Color bgColor;
            Color fgColor;

            if (_hovering) {
                bgColor = _accentColor;
                fgColor = Color.Black;
            } else if (_highlight) {
                bgColor = _accentColor;
                fgColor = Color.Black;
            } else {
                bgColor = Color.FromArgb(20, 20, 28);
                fgColor = _accentColor;
            }

            using (SolidBrush bgBrush = new SolidBrush(bgColor)) {
                g.FillRectangle(bgBrush, ClientRectangle);
            }

            using (Pen borderPen = new Pen(_accentColor, 1.5f)) {
                g.DrawRectangle(borderPen, 0, 0, Width - 1, Height - 1);
            }

            TextFormatFlags flags = TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter;
            TextRenderer.DrawText(g, Text, Font, ClientRectangle, fgColor, flags);
        }

        protected override void OnMouseEnter(EventArgs e) {
            _hovering = true;
            Invalidate();
            base.OnMouseEnter(e);
        }

        protected override void OnMouseLeave(EventArgs e) {
            _hovering = false;
            Invalidate();
            base.OnMouseLeave(e);
        }
    }

    // ───────────────────────────────────────────────────────────────
    //  Owner-drawn dark-themed TabControl
    // ───────────────────────────────────────────────────────────────
    public class DarkTabControl : TabControl {
        private static readonly Color BgColor = Color.FromArgb(15, 15, 22);
        private static readonly Color CyanAccent = Color.FromArgb(0, 229, 255);
        private static readonly Color TabIdle = Color.FromArgb(90, 90, 110);

        public DarkTabControl() {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer, true);
            DrawMode = TabDrawMode.OwnerDrawFixed;
            SizeMode = TabSizeMode.Fixed;
            ItemSize = new Size(110, 28);
            Padding = new Point(0, 0);
        }

        protected override void OnPaint(PaintEventArgs e) {
            Graphics g = e.Graphics;
            g.Clear(BgColor);

            // Draw tab headers
            for (int i = 0; i < TabCount; i++) {
                Rectangle tabRect = GetTabRect(i);
                bool selected = (SelectedIndex == i);

                Color textColor = selected ? CyanAccent : TabIdle;
                using (Font tabFont = new Font("Segoe UI", 8f, FontStyle.Bold)) {
                    TextFormatFlags flags = TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter;
                    TextRenderer.DrawText(g, TabPages[i].Text, tabFont, tabRect, textColor,
                                          flags | TextFormatFlags.NoPrefix);
                }

                if (selected) {
                    using (Pen underline = new Pen(CyanAccent, 2f)) {
                        g.DrawLine(underline, tabRect.Left + 4, tabRect.Bottom - 1, tabRect.Right - 4, tabRect.Bottom - 1);
                    }
                }
            }

            // Draw tab page area border
            if (TabCount > 0) {
                Rectangle pageArea = new Rectangle(0, ItemSize.Height, Width - 1, Height - ItemSize.Height - 1);
                using (Pen borderPen = new Pen(Color.FromArgb(30, 30, 45), 1f)) {
                    g.DrawRectangle(borderPen, pageArea);
                }
            }
        }

        protected override void OnDrawItem(DrawItemEventArgs e) {
            // Handled in OnPaint
        }
    }

    // ───────────────────────────────────────────────────────────────
    //  Double-buffered Panel for flicker-free drawing
    // ───────────────────────────────────────────────────────────────
    public class DoubleBufferedPanel : Panel {
        public DoubleBufferedPanel() {
            DoubleBuffered = true;
            SetStyle(ControlStyles.ResizeRedraw, true);
        }
    }

    // ───────────────────────────────────────────────────────────────
    //  Double-buffered FlowLayoutPanel for scroll content
    // ───────────────────────────────────────────────────────────────
    public class DoubleBufferedFlowPanel : FlowLayoutPanel {
        public DoubleBufferedFlowPanel() {
            DoubleBuffered = true;
            SetStyle(ControlStyles.ResizeRedraw, true);
        }
    }

    // ───────────────────────────────────────────────────────────────
    //  Main Form
    // ───────────────────────────────────────────────────────────────
    public class MainForm : Form {
        // Single source of truth for this build's version. Compared against the
        // remote version.txt to decide whether to show the update notification,
        // and shown in the version label. Keep in sync with version.txt and
        // src/core/version.h on every release.
        private const string APP_VERSION = "3.19.3";

        private string iniPath;
        private Dictionary<string, SettingItem> settingsMap;

        // UI references
        private Label versionLabel;
        private DarkButton btnLogging;
        private Label activeCountLabel;
        private DoubleBufferedPanel progressBarPanel;
        private DarkTabControl tabs;
        private ToolTip toolTip;


        private FlowLayoutPanel generalFlow;
        private FlowLayoutPanel uiLuaFlow;
        private FlowLayoutPanel combatNetFlow;
        private FlowLayoutPanel graphicsSoundFlow;
        // Two tabs that collect by what a switch is rather than by which part of
        // the game it touches. Everything the preset buttons deliberately leave
        // off used to be scattered across the four section tabs, so "why is this
        // one still off" had no place to be answered.
        private FlowLayoutPanel triedFlow;
        private FlowLayoutPanel diagFlow;
        private TextBox searchBox;

        // Background image
        private Image backgroundImage;

        // Drag support
        private bool dragging;
        private Point dragStart;

        // Colors
        private static readonly Color DarkBg = Color.FromArgb(15, 15, 22);
        private static readonly Color DarkerBg = Color.FromArgb(12, 12, 18);
        private static readonly Color CyanAccent = Color.FromArgb(0, 229, 255);
        private static readonly Color PanelBg = Color.FromArgb(18, 18, 28);
        private static readonly Color SeparatorColor = Color.FromArgb(30, 30, 45);
        private static readonly Color SubtextColor = Color.FromArgb(150, 150, 180);
        private static readonly Color SubHeaderColor = Color.FromArgb(150, 150, 180);

        // Must agree with Config::ResolveIniPath in src/core/config.cpp. The two
        // used to be written separately and only matched because the install
        // instructions put this launcher in the game folder; with WTF in the
        // search order, two independent implementations would drift for certain,
        // and the symptom would be the launcher editing a file the DLL never
        // reads. Keep them in step.
        private static string ResolveIniPath() {
            string env = Environment.GetEnvironmentVariable("WOW_OPT_CONFIG");
            if (!string.IsNullOrEmpty(env) && File.Exists(env)) return env;

            string root = AppDomain.CurrentDomain.BaseDirectory;
            string wtfDir   = Path.Combine(root, "WTF");
            string wtfPath  = Path.Combine(wtfDir, "wow_opt.ini");
            string rootPath = Path.Combine(root, "wow_opt.ini");

            if (File.Exists(wtfPath)) return wtfPath;

            // The DLL migrates on the next launch. The launcher only reads
            // whichever file is live today, so a player who opens it before
            // launching once still sees their real settings.
            if (File.Exists(rootPath)) return rootPath;

            return Directory.Exists(wtfDir) ? wtfPath : rootPath;
        }

        // version.dll existing is not the same as version.dll being ours.
        // ReShade and several other mods install their own proxy under that
        // exact filename, and whichever one is written last wins. Everything
        // here used to read the file's presence as the optimizer being active,
        // so a folder whose version.dll belongs to another mod reported ACTIVE
        // while nothing ever loaded wow_optimize.dll. The marker is a string
        // only our proxy carries.
        private const int ProxyUnknown = -1;
        private const int ProxyForeign = 0;
        private const int ProxyOurs = 1;
        private const string ProxyMarker = "wow_optimize_proxy.log";

        private static int ProxyIdentity(string path) {
            byte[] data;
            try {
                data = File.ReadAllBytes(path);
            } catch {
                return ProxyUnknown;
            }
            byte[] want = Encoding.ASCII.GetBytes(ProxyMarker);
            int last = data.Length - want.Length;
            for (int i = 0; i <= last; i++) {
                int j = 0;
                while (j < want.Length && data[i + j] == want[j]) j++;
                if (j == want.Length) return ProxyOurs;
            }
            return ProxyForeign;
        }

        // Our proxy writes this file on every launch of the game, with OK and
        // the path it loaded or with the Win32 error that stopped it. Nothing
        // read it, so a payload that failed to load looked from the outside
        // exactly like the optimizer being switched off.
        private static string LastProxyResult(string baseDir) {
            try {
                string p = Path.Combine(Path.Combine(baseDir, "Logs"), "wow_optimize_proxy.log");
                if (!File.Exists(p)) return null;
                string[] lines = File.ReadAllLines(p);
                for (int i = 0; i < lines.Length; i++) {
                    string t = lines[i].Trim();
                    if (t.Length > 0) return t;
                }
            } catch {
            }
            return null;
        }

        // Windows compatibility settings on an executable in this folder.
        //
        // The README has said for a long time that "Disable fullscreen
        // optimizations" on Wow.exe stops the proxy from loading. A user found
        // that only after moving every other mod out of the folder, because
        // nothing here looked. Windows keeps those settings per executable path
        // under AppCompatFlags\Layers, in the user's hive and in the machine's.
        // Every exe in this folder is matched rather than Wow.exe alone, because
        // private-server clients get renamed.
        private static string CompatLayersInFolder(string baseDir) {
            const string layersKey = @"Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers";
            string dir = baseDir.TrimEnd('\\', '/');
            RegistryHive[] hives = new RegistryHive[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine };
            RegistryView[] views = new RegistryView[] { RegistryView.Registry64, RegistryView.Registry32 };
            foreach (RegistryHive hive in hives) {
                foreach (RegistryView view in views) {
                    try {
                        using (RegistryKey root = RegistryKey.OpenBaseKey(hive, view))
                        using (RegistryKey key = root.OpenSubKey(layersKey)) {
                            if (key == null) continue;
                            foreach (string exe in key.GetValueNames()) {
                                try {
                                    if (!exe.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) continue;
                                    string exeDir = Path.GetDirectoryName(exe);
                                    if (exeDir == null) continue;
                                    if (!string.Equals(exeDir.TrimEnd('\\', '/'), dir, StringComparison.OrdinalIgnoreCase)) continue;
                                    string flags = key.GetValue(exe) as string;
                                    if (string.IsNullOrEmpty(flags)) continue;
                                    return Path.GetFileName(exe) + " (" + flags.Trim() + ")";
                                } catch {
                                }
                            }
                        }
                    } catch {
                    }
                }
            }
            return null;
        }


        public MainForm() {
            // Setup Paths
            iniPath = ResolveIniPath();

            // Tooltip component
            toolTip = new ToolTip();
            toolTip.OwnerDraw = true;
            toolTip.InitialDelay = 250;
            toolTip.AutoPopDelay = 30000;
            toolTip.ReshowDelay = 100;
            toolTip.Draw += ToolTip_Draw;
            toolTip.Popup += ToolTip_Popup;

            // Define settings mapping
            settingsMap = new Dictionary<string, SettingItem>() {
                // General
                { "Precise Sleep Frame Pacing", new SettingItem("General", "SleepPrecision", true, null, "Enforces millisecond-accurate frame-rate sleep pacing to reduce input lag and stabilize frame delivery.") },
                { "Keep a Log File per Session", new SettingItem("General", "SessionLogs", true, null, "Writes a separate timestamped log for every session, so two runs can be compared. Older ones are deleted automatically (SessionLogsToKeep in wow_opt.ini, default 10). Turn off to keep only the single overwritten wow_optimize.log.") },
                { "Lua Allocation Census", new SettingItem("UI_Lua", "LuaAllocCensus", false, null, "Counts every object the Lua VM allocates and reports the size distribution at the end of your log. A measurement, not a speed-up: it decides whether giving Lua its own memory arena would be worth building. Turn it on for one session, send the log, turn it back off.", true) },
                { "SSE2 Terrain Horizon", new SettingItem("Graphics_Sound", "HorizonOcclusionSse2", false, null, "Vectorises the terrain horizon builder, measured at 2.46% of main-thread time in a tester profile. It rasterises up to 384 screen columns one at a time; this does four per instruction. It checks itself: the first 512 calls run both this and the client's version and compare all 384 output values exactly, and any single difference hands every later call back to the client and names the column in your log.", true) },
                { "Shadow State Probe", new SettingItem("Graphics_Sound", "ShadowStateProbe", false, null, "For the shadow flicker some people see when Shadow Quality is set below the highest step. That is not caused by this DLL - a tester reproduced it with every feature here switched off and without DXVK - but nobody has ever looked at what the game itself is doing when it happens. This watches the client's own shadow state and writes what it sees to your log every ten seconds. It changes nothing. Turn it on, play a few minutes with the setting that breaks for you, send the log, turn it off.", true) },
                { "Lua Compile Census", new SettingItem("UI_Lua", "LuaCompileCensus", true, null, "Counts what the game compiles while you play, and names it. About 5% of the client's CPU in one measured session was spent inside the Lua compiler - not because the game compiles a lot, but because one addon was building code in a loop instead of once. Nothing had ever been able to say which. On by default and completely silent on a healthy client; if your log starts listing something with a five-figure count, that is the addon to update or drop, and it is worth several percent of your frame rate.") },
                { "Addon CPU Profiler", new SettingItem("UI_Lua", "AddonProfiler", false, null, "Answers \"which of my addons is eating the frame rate\". The client has a script profiler built in that nothing in the interface ever switches on; this switches it on and writes a ranked table to your log every two minutes - each addon, its milliseconds, and its share. It costs you frames while it is on: collecting the totals walks every addon, and the client's own accounting is not free either. Turn it on for one session when the game stutters, send the log, turn it back off. If every addon reads zero, type /reload once. On a client with no scriptProfile setting it says so and switches itself off instead of costing you anything.", true) },
                { "SSE2 String Compare", new SettingItem("Graphics_Sound", "StrncmpSse2", true, null, "Replaces the client's strncmp, which compares one character at a time, with one that compares sixteen at a time. Measured at 1.55% of the client's CPU time. 3.9x faster on a long comparison and still faster on a short one. It produces the same answer for every input - checked against the client's own routine at startup and against 400,000 cases offline - and it will not read past the end of a string into memory it should not touch, which is the mistake this kind of replacement usually makes.") },
                { "Render Null Guard", new SettingItem("Graphics_Sound", "RenderNullGuard", true, null, "Stops the client crashing when it sets up a model's draw parameters before the render device is ready. It can only do that by skipping the call, and a skipped call draws that model with the previous model's parameters - which looks like a brief flicker. On by default. If you see the screen flicker occasionally, especially after changing a graphics setting, turn this off for one session and say whether it stops; your log now counts how often it fires either way.") },
                { "Animation Census", new SettingItem("Graphics_Sound", "AnimCensus", false, null, "Counts what the model animation update does per frame: how many models, how many bones between them, and microseconds per model. A measurement, not a speed-up. The animation family is about a fifth of main-thread time and is the largest remaining target, but forty models at thirty bones and four hundred at three want completely different fixes and a profile cannot tell them apart. It also settles whether each model brings its own position to that call, which decides whether distance-based animation LOD is possible at all. Turn it on for one session, send the log, turn it off.", true) },
                { "Draw Call Census", new SettingItem("Graphics_Sound", "DrawCensus", false, null, "Counts how many draw calls the client issues per frame, how big they are, and how many of them could have been issued as one. Answered on 2026-09-05: 2.4% of 293 million, and a build that merged exactly those ran slower. Still useful for the per-frame count and the size distribution - same triangle list, same vertex base, next indices along, no state change in between. The last session showed 355 draws a frame with 35% of them carrying eight triangles or fewer, which is where the cost is a call rather than the triangles. Whether merging them is worth building depends on that share and nobody has ever counted it. A measurement, not a speed-up: it wraps the busiest call in the renderer, so run one session, send the log, and turn it back off.", true) },
                { "SSE2 Matrix Multiply", new SettingItem("Graphics_Sound", "MatrixMultiplySse2", true, null, "Replaces the client's 4x4 matrix multiply, which is 199 x87 instructions and runs once per bone per frame on every animated model. Measured at 24.81 ns against 10.43 ns for this one, and the results are bit-identical - worst difference 0.000e+00 over 4096 random matrix pairs, because it accumulates at the same 53-bit width the client does. A single-precision version was 6.5x faster and drifted by 1e-04, which is the order that caused camera snapping once before, so it was not used. On startup the client's own version and this one are run on the same matrices and compared; any disagreement and it refuses to install and says so in the log.") },
                { "Compatibility Mode (only if you need it - turns optimizations OFF)", new SettingItem("General", "CompatMode", false, null, "Leave this OFF unless the game will not connect with the DLL loaded, which usually means a VM or HyperV/virtual switches. It works by SWITCHING OFF optimizations - the CPU-priority, affinity and working-set tweaks that can starve the network in virtualized environments - so it makes the game slower on purpose. It is a repair for a broken connection, not an improvement, which is why Enable All leaves it alone.", true) },
                { "SSE2 Quaternion Normalize", new SettingItem("Graphics_Sound", "QuatNormalizeSse2", true, null, "Replaces the client's quaternion normalize, measured at 3.13% of main-thread execution in a CPU-bound profile. Twice as fast, and it now produces exactly the same bits rather than being a ULP away, so it cannot change how anything looks. It was off by default while it was written in single precision; the client works in double, and that version disagreed with it on three quarters of the quaternions it touched. On startup it runs the client's own version and this one on the same inputs and refuses to install on a single differing bit.") },
                { "Adaptive Quality Governor", new SettingItem("Graphics_Sound", "QualityGovernor", false, null, "Gives up particle density, then shadow quality, then draw distance while the frame-time tail shows the machine cannot keep up, and restores your own settings when it recovers. Learns your values by watching the game write them, and never goes above what you chose. Replaces the three separate scalers. Off by default and skipped by Enable All.", true) },
                { "Lua Stack API Fast Paths", new SettingItem("UI_Lua", "LuaStackFast", false, null, "Replaces 16 core Lua stack functions, including lua_remove, lua_insert and lua_replace. Off by default and skipped by Enable All: it is under investigation for addon errors where an argument arrives missing or wrong.", true) },
                { "Memory Pressure Governor", new SettingItem("General", "MemoryPressure", true, null, "Sheds caches and adjusts texture footprint dynamically under critical 32-bit virtual address (VA) space limits.") },
                { "Heap Compactor", new SettingItem("General", "HeapCompactor", true, null, "Defragments the client heap every 5 seconds to prevent Out-Of-Memory (OOM) crashes during teleports.") },
                { "Lock-Free Heap Defragmenter", new SettingItem("General", "DefragLf", false, null, "Experimental defragmentation on the main thread using lock-free structures. Bypasses standard heap serialization. Skipped by Enable All: experimental, and it bypasses heap serialization.", true) },
                { "Async Worker Pool", new SettingItem("General", "AsyncWorkerPool", false, null, "Background worker threads used by the async subsystems. Also gated by the Lock-Free Heap Defragmenter until now, for no reason anyone recorded. Inherits that setting when its own key is absent. Forced off under Wine and Rosetta, where the workers blocked the main thread.") },
                { "Thread Affinity", new SettingItem("General", "ThreadAffinity", false, null, "Pins client threads to cores chosen from the CPU topology. The third thing the Lock-Free Heap Defragmenter used to gate. Inherits that setting when absent, and Compatibility Mode still overrides it off.") },
                { "D3D9Ex Vulkan DXVK Support", new SettingItem("General", "VulkanDXVK", false, null, "Optimizes DLL hook integration to work cleanly with DXVK (requires placing a d3d9.dll Vulkan wrapper in the game folder).") },
                { "Skip Redundant Graphics State", new SettingItem("Graphics_Sound", "RenderStateDedup", false, null, "The game sets the same graphics state over and over: the same blend mode, the same texture stage settings, the same sampler filters, thousands of times a frame, most of them changing nothing. Each one is still a call into the graphics driver, and under a Vulkan wrapper it is a translation on top of that. This remembers what the state already is and drops the calls that would not change it. It used to be switched on only if you had ticked DXVK support, which has nothing to do with it, so a player on plain Direct3D could not have it; it has its own switch now and keeps whatever it was doing for you before. The log reports how many calls it removed.") },
                { "Windows API Caches", new SettingItem("General", "TimingFix", false, null, "Caches the answers to Windows calls the client repeats constantly and that never change during a session: GetProcAddress, the module file name, environment variables, registry reads, system metrics, the OS version, system info and INI reads. Pure lookups, no game code touched.\n\nThis switch used to be called \"High-Precision Timing Fix\" and its description said it redirected GetTickCount and timeGetTime to the performance counter. It does not, and has not for some time - those three timer hooks, and the QPC coalescing cache with them, are compiled out of the build entirely after they were found to cause random stutters under DXVK. What was left behind the switch was these eight caches, which have nothing to do with timing, so a player chasing a timing bug turned off eight caches instead and a player wanting smoothness turned eight caches on. Reported by biship in #50, who read the code and was right about all of it.") },
                { "Timing CVar Pin", new SettingItem("General", "TimingCvarPin", true, null, "Pins timingMethod to 2 and timingTestError to 0 whatever the client asks for. This has been on for everyone for a long time with no switch, buried inside the CVar safeguard; it now has its own. Leave it on unless you want the client's own timer choice back.") },
                { "Client Crash Guards", new SettingItem("General", "CvarNullGuard", true, null, "Six guards against known client crashes, not one. It declines CVar writes through an object that looks uninitialised - which is what the option used to be named after - and it also wraps the Lua table read at 0x84x, the GUID type check that crashes on battleground load, the object reaper's null write on unlink, and two more null and bounds checks. Turning it off turns off all six, which the old name did not say. On by default. The timing CVar pin that used to ride along inside this feature has its own switch above.") },
                { "WoW.exe Hooks: Core (20)", new SettingItem("General", "WowOptHooks", true, null, "Twenty hooks into the client's own functions: memory copies, object destruction, file reads, error handling. These four groups were installed no matter what you set here - a log with every switch in this launcher turned off still showed about 150 detours in WoW.exe. That is fixed, and these switches are what \"Disable All (vanilla)\" now uses to actually mean vanilla. They default on because they have always been running for everyone, so leaving them alone changes nothing.") },
                { "WoW.exe Hooks: Performance (20)", new SettingItem("General", "WowPerfHooks", true, null, "Twenty more hooks into the client, on the Lua C-API and packet paths. See the group above for why this switch exists. Default on.") },
                { "WoW.exe Hooks: Extended (40)", new SettingItem("General", "WowExtendedHooks", true, null, "Forty further hooks into the client. Your log reports how many of them actually installed, as \"[EXTENDED] N/40\". Default on.") },
                { "WoW.exe Hooks: Subsystem (100)", new SettingItem("General", "WowSubsystemHooks", true, null, "The largest batch: a hundred hooks across file loading, DBC reading, models and the rest of the client's subsystems. Your log reports how many installed, as \"[SUBSYSTEM] N/100\". If you are trying to work out whether this DLL is behind something, this is the biggest single thing to turn off. Default on.") },
                { "D3D9 Render State Dedup", new SettingItem("Graphics_Sound", "D3d9StateManager", true, null, "Patches sixteen entries of the Direct3D 9 device's function table so repeated render-state changes with the same value are dropped instead of going to the driver. Like the groups above it had no switch until 3.18.2 and patched the vtable on every install. Default on. Turn it off if you are testing whether this DLL interacts with an overlay, a capture tool or DXVK.") },
                { "Critical Section Spin Tuning", new SettingItem("General", "LockTuning", true, null, "Gives fifteen of the client's own locks a spin count before they fall back to the kernel. Like the four groups above, this had no switch at all until 3.18.2 and ran on every install regardless of what you set. The hook that does the same for locks created later is the next switch. Default on.") },
                { "Critical Section Hook (All Modules)", new SettingItem("General", "LockTuningInitHook", false, null, "Hooks InitializeCriticalSection in ntdll so every lock created after startup gets a spin count. Every module in the process goes through that hook, overlays and ReShade included, and No Client Patches does not stop it because it is outside WoW.exe. With it on, a player running ReShade could not enter the world; with it off and Critical Section Spin Tuning still on, he could. No session has measured any gain from it. Default off.") },
                { "System Hooks: WoW Only", new SettingItem("General", "SystemHooksClientOnly", true, null, "Many of this DLL's hooks sit on Windows functions (timers, Sleep, window and cursor queries, registry and file lookups, string conversion) and every module in the process calls through them, ReShade, DXVK and overlays included. Most cache an answer or change what the function does. With this on they do that only for WoW.exe and this DLL, and every other module gets the real Windows function. Default on. Turning it off restores the old behaviour, where every module got the cached answers.") },
                { "Background MPQ I/O Worker", new SettingItem("General", "AsyncMpqIo", true, null, "Starts a background thread that reads MPQ data ahead of the main thread. This one is worth knowing about because it is a worker thread, and worker threads are where this project's freezes have come from. It also had no switch until 3.18.2. Default on.") },
                { "Process Priority Guard", new SettingItem("General", "PriorityGuard", true, null, "Hooks SetPriorityClass so nothing can quietly drop the game's process priority back down after it has been raised. No switch until 3.18.2. Default on.") },
                { "Device Callback List Guard", new SettingItem("General", "DeviceCbGuard", true, null, "Fixes a crash where the game executes address 0 and dies instantly. Two people reported it independently - one swapping warrior stances, one alt-tabbing - and both logs land on the same instruction, a call through a callback pointer the client stores in a list and never checks for null. This looks at that list before the client walks it. On a healthy client that is one read-only pointer walk each time the graphics device is torn down, and nothing else happens; if it ever does find a bad entry it writes the whole entry to your log, which is the first time anyone will have seen one. Leave it on.") },
                { "Frame Rate Limiter Override", new SettingItem("General", "FrameLimiter", false, null, "Overrides WoW's built-in frame limiter with a high-precision spin-wait sleep loop.") },
                { "32-bit OOM VRAM Governor", new SettingItem("General", "OomGovernor", false, null, "Dynamically downscales texture mipmaps when the 32-bit client's virtual address space usage approaches critical OOM levels.") },
                { "Hardware Cursor Fix", new SettingItem("General", "HardwareCursor", false, null, "Resets cursor visibility and releases any cursor clip region on startup (no engine byte patches, no hooks). Helps if the cursor is hidden or trapped after alt-tab.") },
                { "Mouse Clip Release on Alt-Tab", new SettingItem("General", "MouseClipRelease", false, null, "Frees the mouse cursor whenever WoW loses window focus, so it is never trapped inside the game window after alt-tab. Polls focus each frame and only ever RELEASES the clip (never applies one), so it cannot cause cursor/camera issues.") },
                { "SavedVariables Backup on Startup", new SettingItem("General", "SavedVarsBackup", false, null, "At startup, copies each WTF\\Account SavedVariables .lua to a .lua.bak so you have the last-good config if a session corrupts it. Runs once on a background thread; only ever copies existing files, never modifies your live SavedVariables.") },
                { "Sampling Profiler (diagnostic)", new SettingItem("General", "SamplingProfiler", false, null, "Developer tool: a background thread samples the main-thread instruction pointer ~1000x/sec and logs the top 50 hot functions on exit. Read-only, no gameplay effect. Leave off for normal play. Skipped by Enable All: it is a diagnostic and it costs frames. One reporter traced their long loading screens to leaving it on.", true) },
                { "Catch Freezes", new SettingItem("General", "FreezeCatcher", false, null, "The game sometimes stops for a moment - a tester session has a frame that took nearly two seconds, and a hundred and forty over a tenth of a second - and nothing in the log can say what it was doing. The recorder tracks file reads, archive opens and network traffic, and during that two-second frame not one of them moved, so whatever it was, it was the processor working on something nobody is watching. The full profiler could answer it but costs too much to leave on, and one reporter traced longer loading screens to having it enabled. This watches instead: a background thread glances at the clock a few times a millisecond and does nothing at all unless the frame already in progress has run past a sixteenth of a second. Only then does it start looking, and only until that frame ends. A frame that behaves costs nothing. Turn it on if you get freezes and send the log.", true) },
                { "No Client Patches (diagnostic)", new SettingItem("General", "NoClientPatches", false, null, "Writes nothing into WoW.exe, which turns every optimization off. Fixes the WoWCircle disconnects: two players ran it and the drops stopped. It is a trade, not a fix - you keep your connection and lose the performance work.", true) },
                { "Flight Recorder (mark a moment)", new SettingItem("General", "FlightRecorder", true, null, "Keeps the last 512 frames and writes 240 of them to the log when you press Scroll Lock. Press it the moment you see something wrong. Nothing is written until you do, and it also marks itself for a disconnect, a freeze and a bad SavedVariables filename. Change the key with FlightRecorderKey in wow_opt.ini.") },
                { "Camera Replay Benchmark", new SettingItem("General", "CameraReplay", false, null, "Measurement only. Stand still somewhere, press Shift+Pause, move the camera around, and press Shift+Pause again: the camera's motion is saved. Press Pause to play it back while the log measures every frame of the playback on its own. Run it once per build or setting from the same spot, facing the same way, with vsync off, and compare the BENCHMARK WINDOW blocks. Only the camera is replayed; other players and NPCs still move, so repeat each side. Hooks one wow.exe function, so leave it off on servers that kick for client patches. Change the key with CameraReplayKey in wow_opt.ini.", true) },
                { "A/B Test a Feature", new SettingItem("General", "AbTest", false, null, "Turns one feature on and off in stints while you play and compares the two halves. It can only measure features you have switched on. Tick the features you want compared as well, or it has nothing to measure. Play at least 45 minutes.", true) },
                { "Thread ID Cache", new SettingItem("General", "ThreadIdCache", true, null, "Caches GetCurrentThreadId in a thread-local slot to avoid redundant kernel queries on hot paths.") },
                { "Object Visibility Lookup Cache", new SettingItem("General", "ObjVisCache", false, null, "Caches GUID-to-object visibility hash lookups in a thread-safe slot pool. Experimental, off by default.", true) },

                // UI & Lua
                { "Fast UI Frame Accessors", new SettingItem("UI_Lua", "UIFrameAccessorFast", false, null, "Bypasses standard Lua stack queries to retrieve UI frame parameters (IsShown, GetAlpha) instantly.") },
                { "Fast FontString Metrics & Glyph Cache", new SettingItem("UI_Lua", "FontMetricsFast", false, null, "Provides high-speed text measurements and caches rasterized font glyph textures to eliminate render-time layout freezes.") },
                { "FrameScript FNV-1a Dispatcher", new SettingItem("UI_Lua", "FrameScriptDispatch", false, null, "Uses an O(1) hash map lookup for script handlers instead of linear string matching.") },
                { "Lua Number Conversion Fast Path", new SettingItem("UI_Lua", "LuaNumConvFast", false, null, "Inlines common Lua stack value queries (tonumber, gettop, settop) to bypass stack checking overhead.") },
                { "Lua GetTime Frame Cache", new SettingItem("UI_Lua", "LuaGetTimeFast", false, null, "Caches the GetTime() Lua API value within a single frame tick to avoid redundant OS-level high-precision timer calls.") },
                { "UI Layout Relink Shortcut", new SettingItem("UI_Lua", "LayoutRelinkFast", false, null, "The biggest single thing left. When the game re-anchors a UI frame it searches the entire global layout list, dereferencing up to nine pointers per frame, looking for anything anchored to the one that moved. In a 28 minute session with ElvUI that search was 9.06% of all main-thread CPU time, first place by more than double, and it gets worse the more frames your addons create.\n\nThe game already keeps the answer: each frame has a list of what is anchored to it. If that list is empty the search cannot possibly find anything, and empty is the expensive case, because finding nothing means having walked everything.\n\nOff by default and it earns its way on: for the first 20,000 calls it changes nothing, it only predicts and then checks what the client actually did. It starts taking the shortcut after 20,000 agreements, keeps checking one call in 1024, and switches itself off for the session on a single disagreement. An earlier attempt at this crashed on login; that one wrote to a client global, this one does not.", true) },
                { "Object List Walk", new SettingItem("UI_Lua", "ObjMgrEnumFast", false, null, "Twenty-five places in the game ask what objects are nearby - targeting, nameplates, threat, spell visuals - and each one walks the whole object list, calling back once per object. For every single object on that walk the game rebuilds the address it needs to find the next one, two memory reads that have to finish before the read that fetches the next object can even start, for an answer that is the same for the entire walk. This works it out once. The list itself is walked live, exactly as the game walks it, in the same order - nothing is cached or copied, so nothing can go stale. It also counts what the walk costs: how many times a frame it runs, how many objects each one visits, and the longest. Those numbers are the point as much as the saving is, because nobody has ever measured this.", true) },
                { "Lua VM Optimizer", new SettingItem("UI_Lua", "LuaVmOpt", true, null, "Replaces the Lua VM's own allocator with mimalloc, pre-sizes its string table and retunes its garbage collector. This is one of the largest things this DLL does to the game and it had no switch at all until 3.18.2 - it ran on every install regardless of the launcher, including with everything turned off. Default on, because that is what everyone has been running.") },
                { "Lua VM: stop the automatic GC", new SettingItem("UI_Lua", "LuaGcManual", true, null, "Part of the optimizer above, split out because it is the part worth testing on its own: it stops the VM's automatic collector and steps it by hand instead. That changes when memory is reclaimed, which is the first thing to suspect for a session that gets progressively worse the longer it runs. Turn this off to hand collection back to the VM while leaving the rest of the optimizer alone.", true) },
                { "Lua C-API Inline Cache Suite", new SettingItem("UI_Lua", "LuaOpcache", false, null, "Master switch for the Lua C-API fast paths. Off by default. It gates fifty-five separate hooks, which is why the four switches below exist: a tester reported that this suite corrupts ElvUI - addon names come out wrong in the addon list, the options panel reports itself missing, and a /reload drops you to the default Blizzard UI - and with everything behind one checkbox there was no way for them or for me to narrow it to one hook. Turn this on, then turn the groups below off one at a time until the corruption stops, and send the log. Leaving all four on is identical to how this switch behaved before. Skipped by Enable All: issue #37 reported it lengthening load times and producing Lua errors.", true) },
                { "Lua Suite: table & index caches", new SettingItem("UI_Lua", "LuaOpcacheTables", true, null, "Part of the suite above, and the first group to suspect: the global, table, index and luaH_getstr caches, plus the VM table indexing path. These are the hooks that can hand back a value for the wrong key, which is what wrong addon names look like. Only has any effect when the suite above is on.", true) },
                { "Lua Suite: string & buffer paths", new SettingItem("UI_Lua", "LuaOpcacheStrings", true, null, "Part of the suite above: pushstring, pushfstring, the string buffer helpers, tolstring, loadstring and the compiled pattern cache. Second group to suspect for wrong text. Only has any effect when the suite above is on.", true) },
                { "Lua Suite: accessors, arg checks & debug", new SettingItem("UI_Lua", "LuaOpcacheReads", true, null, "Part of the suite above, and the least likely group: type queries, length, toboolean, the luaL_check/opt argument helpers, and the debug and error helpers. Mostly read-only. Only has any effect when the suite above is on.", true) },
                { "Lua Suite: setters & object creation", new SettingItem("UI_Lua", "LuaOpcacheWrites", true, null, "Part of the suite above: pushcclosure, createtable, rawset, rawseti, settable, setfield, and register/ref fast paths. Only has any effect when the suite above is on.", true) },
                { "Adaptive Lua GC Governor", new SettingItem("UI_Lua", "LuaGcCoalesce", false, null, "Paces incremental garbage collection per frame from the live game state - relaxed while a loading screen is up, stopped in combat below 256MB, aggressive while idle.") },
                { "Module Handle Cache", new SettingItem("UI_Lua", "ModuleHandleCache", false, null, "Caches GetModuleHandle results, which the client queries repeatedly for already-loaded modules.") },
                
                // Combat & Net
                { "Combat Log Filter", new SettingItem("Combat_Net", "CombatLogFilter", false, null, "Drops every combat log event where neither the source nor the target is you, your party or your raid. That is a real reduction in work, but it is not free: arena and battleground opponents fighting each other, boss abilities aimed at other NPCs, and anything else happening outside your group stops reaching addons at all. A damage meter will under-report and an arena addon will not see an opponent's cooldowns. Off by default. It works on its own; until now it silently did nothing unless Event Coalescing was also on, and Event Coalescing was not even listed here.", true) },
                { "Network Packet Reader Fast Paths", new SettingItem("Combat_Net", "SavedVarsPretoken", false, null, "Replaces the six CDataStore accessors the client uses to pull fields out of every network packet - GetDword, PutDword, GetByte, PutByte, PutQword and one more, together about 4,000 call sites.\n\nIt was called \"Saved Variables Pretokenize\" until 3.18.2, and the description admitted it also installed the whole Win32 file-hook suite, a stream cache and a packet batcher. Every one of those turned out to be dead: the pretokenizer's entire implementation was `return false` in each of its six entry points, the stream cache logged \"Disabled\" and returned, the batcher only initialised counters, and the stream-buffer fast path aimed at the same two addresses as the accessors above and always lost the race. What was left doing real work was the accessors, so that is what the switch is now named after and all it now installs.") },
                { "Vertex Buffer Preallocation", new SettingItem("General", "VertexBufferPrealloc", false, null, "Pools vertex buffer allocations for the D3D9 state cache instead of allocating per use. Runs on every install; this switch is new.") },
                { "M2 Matrix SSE2", new SettingItem("Graphics_Sound", "M2MatrixSimd", false, null, "SSE2 matrix copy for model transforms, plus the SIMD bone path. Runs on every install; this switch is new.") },
                { "CRT Alloc Fast Path", new SettingItem("General", "CrtAllocMsize", true, null, "WoW's allocation wrapper calls _msize after every successful allocation and throws the result away, exactly as the free wrapper did - a heap lookup, sometimes a lock, for a number nobody reads. Same fix, applied to the other half. Separate switch from the free one so either can be turned off alone.") },
                { "CRT Free Fast Path", new SettingItem("General", "CrtFreeMsize", true, null, "WoW's free wrapper calls _msize on every deallocation and throws the result away - a heap lookup, sometimes a lock, for a number nobody reads. Two tester profiles measured that call at 8-10% of main-thread execution time. This removes it and changes nothing else.") },
                { "WoW API Result Cache", new SettingItem("Combat_Net", "ApiCache", true, null, "Caches GetItemInfo and GetSpellInfo results. This is the cache behind the WeakAuras icon that stays wrong after a talent switch: it used to be cleared only by a /reload. Entries now expire after a second. Until this release it had no switch at all and ran on every install - turn it off if you still see stale spell data.") },
                { "Disconnect Diagnostics", new SettingItem("Combat_Net", "NetDiag", true, null, "Watches the receive path and writes a report if your connection ends: how it ended (a clean close, a reset, a timeout), how long since the last byte actually arrived, and whether the game was mid-loading or the main thread had stalled. It changes nothing about how the game talks to the server - it only records. Disconnects are the oldest complaint about this DLL and the only one never explained, because nothing was watching. On by default; if you get dropped, your log will now say something about it.") },
                { "Combat Log Leak Fix (retention 1800s)", new SettingItem("Combat_Net", "CombatLogLeakFix", true, null, "Fixes the 16-year-old WoW combat log memory leak by extending event retention from 300s to 1800s (writes the retention CVar). Proven and stable - on by default.") },
                { "Combat Log Aggregator", new SettingItem("Combat_Net", "CombatLogParser", false, null, "C++ combat log aggregator + buffer governor that intercepts and summarizes events instead of the slow Lua path. More aggressive than the leak fix above; opt-in.") },
                { "Incremental Combat Log parsing", new SettingItem("Combat_Net", "CombatLogIncremental", false, null, "Splits large combat updates into small steps, preventing massive spikes in large-scale combat.") },
                { "SSE2 Network GUID Unpacking", new SettingItem("Combat_Net", "NetworkGuidSse2", false, null, "Vectorizes the unpacking of network entity GUIDs inside network data streams.") },

                // Graphics & Sound
                { "SSE2 Boyer-Moore strstr", new SettingItem("Graphics_Sound", "StrStrSse2", false, null, "Optimizes string sub-searches (such as font names, textures) using vectorized SIMD algorithms.") },
                { "SSE2 Frustum Cull and Quaternion Normalize", new SettingItem("Graphics_Sound", "SimdGeometry", false, null, "Replaces the frustum culling and quaternion normalize routines with SSE2 versions. These used to be switched on and off by the SSE2 string search option, which is named after something else entirely, so anyone who left that off - it is off by default - lost these too without being told. If you have never set this key it follows whatever the string search option is set to, so nothing changes for you by updating.") },
                { "Vectorized String Concatenation", new SettingItem("Graphics_Sound", "StrCatFast", false, null, "Speeds up string appending (such as chat text building) using SSE2 assembly wrappers.") },
                { "FMOD Sound Mixer Optimization", new SettingItem("Graphics_Sound", "SoundMixerOpt", false, null, "Adjusts audio thread schedules and buffer allocations to prevent sound stutters in raids.") },
                { "Parallel Sound Wave Decoding", new SettingItem("Graphics_Sound", "AudioDecodeMt", false, null, "Decodes sound assets in background threads to eliminate latency when playing fresh audio clips.") },
                { "DBC Data Lookup Cache", new SettingItem("Graphics_Sound", "DbcLookupCache", false, null, "Speeds up data reading from internal database files (.dbc) for models, items, and spells. Skipped by Enable All: issue #35 reported it crashing the client during a loading screen, and that has not been re-tested since the file hooks were split out of it, so it is not known which half was at fault.", true) },
                { "File I/O Hooks", new SettingItem("General", "FileIoHooks", false, null, "Everything this tool does to Windows file calls: sequential-scan hints on open, the adaptive read cache for MPQ archives, handle cleanup, a skipped buffer flush, and caches for file attributes, seeks and sizes. These used to be switched by the DBC cache above, which meant clearing that one to test it also removed the whole file layer, silently. Turn this off if loading, streaming or disk behaviour looks wrong. Skipped by Enable All for the same reason as the DBC cache it was split from: the crash reported in issue #35 could have come from either half.", true) },
                { "Loading Screen Breakdown", new SettingItem("General", "MpqOpenCensus", false, null, "Answers where a loading screen actually goes. One session in the field took forty-five seconds to load, of which the game spent 89 milliseconds reading from disk and 97 compiling Lua - everything else, over ninety-nine percent of the wait, is unaccounted for, and the data was already in Windows' own file cache so it was not the drive. This counts and times the call the game uses to open a file inside its archives by name, and separates three cases: the file was found, the name was searched for and not there, and the name had already been searched for and was still not there. Only that third case is work something could remove, so it is the number that says whether a fix is worth building. It also names the files asked for most often and never found. Nothing is cached and no call is skipped - this only counts.", true) },
                { "Skip Repeated Missing-File Lookups", new SettingItem("General", "MpqNegativeCache", false, null, "Removes a specific piece of loading-screen work rather than measuring it. The game looks for files inside its archives by name, and a name that is not there is searched for in every archive it has open - base game, every patch, and whatever your server added. It asks for files that do not exist constantly: optional textures, per-race variants, sounds an effect may not have. Asking a second time for a name already searched for and not found is the one part of that which is pure waste, and this answers those without going back to the game. It is awake only while a loading screen is up, and forgets everything when the next one starts, because what exists can only change between loads and not during one. Before it skips a single call it lets the game answer two thousand of them and checks it agrees every time; afterwards one in every 256 is still checked. One disagreement and it stops for the session. Turn on Loading Screen Breakdown with it to see what it saved.", true) },
                { "UI Frame Batch (parent switch)", new SettingItem("UI_Lua", "UIFrameBatch", false, null, "The setting the two below inherit from when they are absent from the file. It used to be read by the tool with no entry here at all, so it could only be changed by editing the ini by hand, and it has been off for everyone since issue #36 reported flickering. It no longer switches anything by itself: the two halves it really controlled have their own entries, and the other two things it appeared to control are compiled out of the build. Leave it off and use the two below.", true) },
                { "Table Emptiness Census", new SettingItem("UI_Lua", "LuaTableCensus", false, null, "Diagnostic, not an optimization. The garbage collector walks every slot of every table it visits, including the empty ones, and a table that once held a thousand entries keeps a thousand slots for as long as nothing new is inserted into it. That function is the most expensive Lua thing in every profile taken so far. This samples one table in five hundred of the collector's walk and reports how many of the slots it stepped over were empty. It only counts and never changes anything. The answer decides whether a compactor is worth writing.", true) },
                { "Leave Lua Garbage Collection Alone", new SettingItem("UI_Lua", "LuaGcStockPace", false, null, "The garbage collector governor normally makes Lua collect more eagerly than it would on its own, to keep memory down and avoid a large pause later. That eagerness has a price the tool has never measured: it is paid inside the game's own collector, and in profiled sessions those two functions are the two most expensive Lua things running, ahead of the script interpreter itself. Tick this to leave the collector exactly as the game sets it and step nothing by hand. Run one session each way and compare the two lines the log prints; that is the entire experiment.", true) },
                { "Object Tick Prefetch", new SettingItem("Graphics_Sound", "TickListPrefetch", false, null, "Every frame the game walks a linked list of objects and pokes each one, and almost every poke does nothing but read two fields and return. Those two fields sit in different cache lines, so each object costs the processor two waits, and it cannot start the next object until it has finished the previous one. This tells the processor to start fetching the next object while the current one is still being handled. It was 1.39% of main-thread time in a measured session. It changes nothing about what the game computes - a prefetch is only a hint - and it refuses to install unless the function is byte-for-byte the one it was written against.", true) },
                { "Terrain Read-Ahead", new SettingItem("General", "TerrainPrefetch", false, null, "Watches where the world is streaming from, projects that forward, and reads the terrain tiles ahead of you out of the MPQ archives on a background thread so they are in the OS cache before the game asks. It read its coordinate from an address the game never writes, so from the day it was added until 3.19.1 it queued nothing at all on any machine - which is also why nobody has ever tested what it does when it works. It does real background disk reads.", true) },
                { "Lua Type Fast Path", new SettingItem("UI_Lua", "LuaTypeFast", false, null, "Resolves a Lua stack index inline in lua_type instead of calling the engine's index2adr. Also used to be switched by the DBC cache, which it has nothing to do with.") },
                { "Win32 API Caches", new SettingItem("General", "Win32ApiCaches", false, null, "Caches Windows calls that return the same answer every time: system info, OS version, registry reads, GetProcAddress, module file names, environment variables and ini reads. Screen metrics used to be on that list and is not any more - it was measured at a zero percent hit rate and taken out. These used to be switched by the timing fix, which should own the clock hooks and nothing else.") },
                { "Debug API Hooks", new SettingItem("General", "DebugApiHooks", true, null, "Answers IsBadReadPtr and IsBadWritePtr from a memory query instead of the slow path, turns OutputDebugString into a no-op, and reports no debugger attached. These used to be switched by the CVar safeguard, which is unrelated and defaults on, so this defaults on too and keeps doing what it already did.") },
                { "Lock Spin Counts", new SettingItem("General", "LockSpinHooks", false, null, "Adds spin counts to CriticalSection and WaitForSingleObject so a short wait does not go straight to the kernel. These used to be switched by the heap defragmenter, a different subsystem.") },
                { "Bone Rotation Maths (SSE2)", new SettingItem("Graphics_Sound", "QuatLerpSse2", false, null, "Every animated bone of every model gets its rotation blended between two keyframes, every frame. The game does the four numbers one at a time on the old floating-point stack; this does all four in one instruction. Not identical to the last bit: measured over 12 million values the largest difference is 0.0000003, which is under three of the smallest steps a float can take, and the result is renormalised straight afterwards. It checks itself against the game for the first 20000 blends and switches off if anything drifts further than rounding explains.", true) },
                { "Reuse Compiled Scripts", new SettingItem("UI_Lua", "LuaProtoCache", true, null, "Interface scripts written inside XML templates are recompiled from scratch every time a frame is built from that template. Counted on real sessions: 88 out of every 100 chunks the game compiled were text it had already compiled that same session, 332 MB of repeated work. This keeps the compiled form and reuses it when the text and the chunk name are both identical, checked byte for byte rather than by a hash. The game still builds the function itself, so its environment and its addon ownership are unchanged. It compares the first 2000 reuses against a fresh compile and switches off if any of them differ.", true) },
                { "Lua Interpreter (experimental)", new SettingItem("UI_Lua", "LuaVmFast", false, null, "Runs the game's Lua interpreter out of this DLL instead of out of WoW.exe, transcribed instruction by instruction from it, with one thing changed: the table lookup behind every global read and every method call is done in place rather than through three nested calls. Anything the transcription does not cover exactly - an arithmetic metamethod, a comparison that is not two numbers, a debug hook - is handed straight back to the game. The inlined lookup is checked against the game's own for its first 65536 hits and one in 1024 after that, and a single disagreement retires it for the session and says so in your log. Off by default: this is the function every line of Lua in the game runs through.", true) },
                { "Reuse Compiled Scripts Between Sessions", new SettingItem("UI_Lua", "LuaBytecodeStore", false, null, "Reuse Compiled Scripts only helps the second time the game compiles something in one sitting. On a measured loading screen that was 260 of the 2128 milliseconds spent compiling; the other 1868 were scripts the session had never seen, which nothing running inside the game can avoid. This writes the compiled form to Cache\\wow_optimize_bytecode.bin and reads it back on the next launch, so a script compiled yesterday is not compiled again today. The game can write that form but has no code to read it, so the reading is ours: every script rebuilt from the file is compared against a real compile of the same text, field by field and constant by constant, for the first 2000 of them and one in every 256 after that, and the whole store switches off for good the first time two of them differ. The file is discarded automatically if Wow.exe changes.", true) },
                { "Collision Box Test (SSE2)", new SettingItem("Graphics_Sound", "CollisionOutcode", false, null, "Every line-of-sight check, every mouse click on the world and every projectile path makes the game sort the corners of a collision model against a box, one corner at a time on the old floating-point stack - six comparisons per corner. A corrected profile puts that single function at 3.8% of main-thread time, the largest one left outside model animation. This does four corners per instruction. Unlike the other maths replacements in this tool it is exact rather than close: the box bounds are read as plain numbers with no arithmetic done to them, so the vector comparison gives the same answer as the game's for every possible input. Before it changes anything it works out what the game is about to produce - which corners are outside and which triangles get queued - lets the game run, and compares the two lists. Three thousand of those have to match before it takes over.", true) },
                { "Collision Model Cache (SSE2)", new SettingItem("Graphics_Sound", "CollisionModelCache", false, null, "Accelerates the 8-way set-associative BSP collision model cache lookup on sub_79B1F0. Uses dual 128-bit SSE2 vector comparisons to test all 8 set slots simultaneously with zero branch mispredictions. Verified against original lookup with automatic miss fallback.", true) },
                { "Collision Ray Test (SSE2)", new SettingItem("Graphics_Sound", "CollisionRayOutcode", false, null, "The other half of the same collision work. A line-of-sight check or a mouse click on the world casts a ray, and before the ray meets a single triangle the game sorts every corner of the model against a box and compares each one six times on the old floating-point stack. Each of those comparisons is a branch on whether a corner is outside one face, which is a coin toss, so a model of a few hundred corners costs a few hundred mispredicted branches. A corrected profile puts the function at 2.6% of main-thread time. This does four corners at a time with no branch in it. The box here is nudged outward by a hundredth of a yard and the game keeps that nudge at a wider precision than a normal number, so the comparisons are done at that wider precision too and give the game's own answer for every possible input. Before it changes anything it computes the whole classification alongside the game's, lets the game run, and compares. Three thousand calls have to match before it takes over.", true) },
                { "Ray vs Triangle Test (SSE2)", new SettingItem("Graphics_Sound", "RayTriangleSse2", false, null, "The third and last piece of the collision work. Once the game has narrowed a ray down to the handful of triangles it might actually touch, it tests each one, and that test is shared by every collision path in the game - line of sight, mouse clicks on the world, projectiles, footing. A corrected profile puts the whole collision family at 6.4% of main-thread time and this is the part of it doing the real arithmetic: 231 old floating-point-stack instructions with five round trips through the status register, each one feeding a branch on whether the ray missed, which the processor cannot predict. This carries the same numbers at the same width and compares them directly. It is a transcription rather than a tidy-up: wherever the game narrows an intermediate value to a smaller number this narrows it too, because writing the same formula cleanly gives a different distance on more than a quarter of hits. Before it answers anything it computes the result alongside the game twenty thousand times and compares the answer and every number written; one call in four thousand keeps checking afterwards.", true) },
                { "Occluder Sphere Test (SSE2)", new SettingItem("Graphics_Sound", "OccluderSphere", false, null, "Accelerates the convex occluder volume sphere culling test on sub_7CCE00. Evaluates occluder planes 4-wide in parallel using transposed SSE2 SIMD dot products instead of serial x87 calculations, eliminating x87 pipeline stalls across world and model visibility passes.", true) },
                { "M2 Animation Track Search", new SettingItem("Graphics_Sound", "M2AnimFindKey", false, null, "Accelerates the M2 animation timeline keyframe search on sub_8284D0. Replaces serialized x87 float divisions and store forwarding stalls with branch-optimized keyframe resolution and SSE math across all bone and model animation tracks.", true) },
                { "Bone Matrix Upload (SSE2)", new SettingItem("Graphics_Sound", "BoneMatrixUpload", false, null, "Replaces the bone matrix transpose in the draw path with SSE2. 3.35% of main-thread time in the profile. Nothing is computed, only copied, so the result is identical bit for bit; it still checks the first 20000 bones against the client and backs out if they differ.", true) },
                { "Particle Vertex Fill (SSE2)", new SettingItem("Graphics_Sound", "ParticleFill", false, null, "Replaces the per-particle vertex fill in the client's particle emitter (2.5% of self time in a combat profile). For every particle the client calls a getter and adds the offset on the x87 stack; this reads the getter once per fill and does the add with SSE, writing the same bytes. It first predicts 4096 fills and compares each byte for byte with what the client itself wrote, keeps comparing one in 1024 after that, and switches itself off for the session at the first difference.", true) },
                { "UI Batch Fill (SSE2)", new SettingItem("Graphics_Sound", "UiBatchFill", false, null, "Replaces the per-vertex fill in the client's UI batch draw (UI_BatchDraw, 2.3% of executing time in a combat profile with a busy interface). For every vertex the client calls a getter and reloads six values; this reads them once per batch and writes the same bytes. It first predicts 4096 batches and compares each byte for byte with what the client itself wrote, keeps comparing one in 1024 after that, and switches itself off for the session at the first difference.", true) },
                { "M2 Matrix Slot Copy (SSE2)", new SettingItem("Graphics_Sound", "M2MatrixSlotSse2", false, null, "Replaces the two places in the model animation update that copy a finished bone matrix into the model one float at a time - sixteen x87 moves each - with four SSE2 moves. There is no arithmetic in either, so the bytes written are the bytes read. The animation family is about a fifth of the frame.", true) },
                { "M2 Batch Matrix Setup (SSE2)", new SettingItem("Graphics_Sound", "M2BatchMatrixSse2", false, null, "Replaces the serialized sixteen-float matrix setup copies in sub_823130 (M2 batch render pass setup) with four SSE2 vector moves each. Eliminates x87 store-forwarding stalls during model rendering.", true) },
                { "M2 Scalar Animation Tracks (SSE2)", new SettingItem("Graphics_Sound", "AnimScalarTrack", false, null, "Vectorizes the packed int16 and float scalar animation track evaluators in sub_82AF40 and sub_82B340 using hardware double-precision SSE2. Handles transparency, alpha channels, camera FOV, and model colors with bit-exact float outputs across keyframe interpolation and blending.", true) },
                { "M2 Spline Animation Tracks (SSE2)", new SettingItem("Graphics_Sound", "AnimSplineTrack", false, null, "Vectorizes the 3D vector and scalar cubic spline animation track evaluators in sub_82B460 and sub_82B8A0 using hardware double-precision SSE2. Evaluates cubic Hermite and Bezier spline tracks for models, ribbons, lights, and cameras with bit-exact float outputs and dual-run verification.", true) },
                { "Model Animation Stride (experimental)", new SettingItem("Graphics_Sound", "M2AnimStride", false, null, "Holds a distant model's skeleton for a frame instead of re-solving every bone. Its materials, particles and attached items keep animating - only the bones pause. Nothing within 45 yards is ever held; past that a model updates every 2nd, 3rd or 4th frame by distance, and each one is on its own phase so they do not all update together. The animation family is about a fifth of the frame.\r\n\r\nReported stuttering visibly on environment animation, the lava in Ironforge among it. Distance is a poor stand-in for whether a held skeleton is seen: a large animation fills the screen at any range. No measured frame gain stands against that yet - the sessions that showed the stutter were frame capped, where a saving inside the frame changes no frame time. It is an A/B subject; that run has not happened.", true) },
                { "Reuse Repeated Animation Poses", new SettingItem("Graphics_Sound", "M2AnimReuse", false, null, "A model's skeleton is rebuilt from scratch every frame, and in one measured session nine times out of ten the game asked for a pose it had already worked out - same model, same animation, same moment - while the game's own check that is meant to catch that fired for none of a quarter million rebuilds. This keeps the pose when the question is word for word the same one, so what you see is the pose the game would have recalculated, not an old one. Everything after the skeleton still runs, so weapons, particles, lights and texture animation carry on. Models whose skeleton carries a running clock are left alone. Before it holds anything it fingerprints the skeleton the game produced and waits for five thousand of those to come out identical; one hold in four thousand keeps checking afterwards. Turn Model Animation Stride off to use this - they cut the game at the same instruction, and that one decides by distance, which is why it stuttered.", true) },
                { "Keep the Allocator Above 2GB", new SettingItem("General", "MimallocHighArena", false, null, "A 32-bit client can only allocate from the low 2GB, and this tool's allocator grows into the same half. Three sessions ran out of it, and one had a SavedVariables file written under a garbage name. This reserves address space above 2GB and hands it to the allocator, which uses memory it is given before asking the OS - and hands over more as it fills, so the allocator never has a reason to come back down. Needs a large-address-aware client. It releases any block Windows places below 2GB rather than use it. Sizes are MimallocHighArenaMB and MimallocHighArenaMaxMB in wow_opt.ini.", true) },
                { "Address Space Census", new SettingItem("General", "VaCensus", false, null, "Measurement only. Records every private address-space reservation by the module that made it - wow.exe, DXVK, the GPU driver, this tool - so the low-2GB dump in the log names who holds that half instead of calling it \"private\". Every out-of-memory report so far has said how much private memory sits below 2GB and never whose it is. Hooks two ntdll allocation entry points; nothing is placed or freed differently.", true) },
                { "Large Reservations Above 2GB: Other Modules", new SettingItem("General", "HighPlacementModules", false, null, "Asks Windows to place reservations of HighPlacementMinKB or more (1024 KB unless set in wow_opt.ini) from DXVK, the GPU driver and other DLLs above 2GB, so they stop using up the contiguous space the client allocates from. Placement only: nothing is redirected. Needs a large-address-aware client. A heap segment it moves up is afterwards used by everything that shares that heap, wow.exe included.", true) },
                { "Large Reservations Above 2GB: wow.exe", new SettingItem("General", "HighPlacementClient", false, null, "The same for wow.exe's own reservations and heap growth. Riskier: the client's large-address-aware flag comes from a community patch, and nobody has checked that every path in it handles pointers above 2GB. If the game misbehaves with this on and not with it off, that is what it found.", true) },
                { "Batch the Game's File Writes", new SettingItem("General", "ClientWriteBatch", true, null, "The game writes SavedVariables about nine bytes at a time. One tester's loading screen spent 2470 ms of 16828 inside 593557 of those calls, for 5.6 MB. This gathers them into 64KB pieces, so the same work is about ninety system calls. It buffers one file at a time, flushes on every close, seek, read and flush, and checks each closed file's size against what the game handed over - if a byte ever goes missing it switches itself off and says so. Needs File I/O Hooks.", true) },
                { "Box Overlap Test (SSE2)", new SettingItem("Graphics_Sound", "AabbOverlap", false, null, "Before drawing anything the game asks, for every object in the scene and for every visibility pass over it, whether that object's box overlaps the one being tested. Seventeen different parts of the engine ask it. The test itself is six number comparisons, but each one is moved off the old floating-point stack through the slowest instruction available for that, and each is followed by a branch the scene data decides - so a walk over a mixed set of objects guesses wrong on most of them. This answers all six at once. Nothing is added or multiplied anywhere in the test, only compared, so the vector version gives the identical answer for every possible input rather than a close one. It checks itself against the game's own answer twenty thousand times before it starts answering alone, and keeps rechecking one call in four thousand after that.", true) },
                { "Bounding Box Transform (SSE2)", new SettingItem("Graphics_Sound", "AabbTransform", false, null, "Transforms an object's bounding box by a 4x4 or 3x3 matrix (sub_7F9430 and sub_7F93D0) during scene graph visibility traversal and culling passes. Replaces Jim Arvo's algorithm on the x87 FPU - which suffers 18 status-word transfers (fnstsw ax) and 9 data-dependent branch mispredictions per box - with hardware double-precision SSE2 registers, achieving bit-exact floating point parity with dual-run verification.", true) },
                { "Colour Pack and Unpack (SSE2)", new SettingItem("Graphics_Sound", "ColorUnpack", false, null, "Replaces the game's colour conversions - packing and unpacking BGRA and BGR, RGB to HSV and back - and the two routines that pick a vector's largest and smallest axis. Each is checked against the game's own routine before it is used and switches itself off on the first difference. Off by default: none of it has been run in a game yet.", true) },
                { "Lua Pool Shortcuts", new SettingItem("UI_Lua", "LuaPoolFast", false, null, "The game keeps its own pool of memory for the interface scripting language, carved into chunks. Every time it hands a block back, it has to work out which chunk that block came from, and it does that by checking every chunk in turn, following a pointer to each one before it can even compare. This remembers the last few chunks along with their boundaries, so the usual answer is a couple of comparisons instead of a walk through scattered memory. The block is always re-checked against the chunk's own record before anything is written, so a stale entry costs a little time and can never put memory in the wrong place. It also tells the separate 'Lua Pool Allocation Hint' feature which chunk just got a block back, which is the one thing that feature could not know on its own: a measurement of a tester's session showed three quarters of allocations finding room immediately but nearly a fifth still searching through thirty-three chunks or more, and that tail is exactly memory freed into a chunk the search had already passed. Two testers' freeze reports have pointed at this code.", true) },
                { "Matrix-Vector SSE2 (slower - off)", new SettingItem("Graphics_Sound", "MatrixVectorSse2", false, null, "Replaces one small piece of the game's 3D maths with a modern instruction set. It is off, and it will stay off unless you have a reason: measured side by side against the game's own code it came out slower - 3.3 against 2.5 nanoseconds a call - while producing exactly the same numbers, and it runs about five thousand times per frame. It was switched on for everyone by accident, tied to an unrelated text-search option, so nobody could turn it off. Left here only so the measurement can be repeated.", true) },
                { "Steadier Shadows (flicker fix)", new SettingItem("Graphics_Sound", "ShadowCascadeHold", false, null, "Below the highest shadow setting the game does not redraw shadows every frame. It builds a new shadow map over nine frames around the point you were standing on when those frames began, then shows the whole thing at once - so the shadows do not fade in, they jump. The nearest map does that every two yards, which while running is about three times a second, and that jump is the flicker people see on buildings as they run past. This halves the distance, so the jumps are half the size and twice as often: many small corrections read as movement where a few large ones read as popping. It costs some frames the game would have skipped the shadow work on entirely. An earlier version went the other way and doubled the distance, which removed the flicker for one tester and gave another visibly lagging shadows. EXPERIMENTAL - this is a judgement about how a jump looks, not a measurement, so try it against having it off.", true) },
                { "Table Lookup Dispatch (SSE2)", new SettingItem("UI_Lua", "LuaHGetDispatch", false, null, "Every time an addon reads a value out of a table, the scripting engine first has to work out what kind of key it was given. Deciding whether a number is a whole number - which is how any list is indexed - costs it three trips through memory and a stall waiting for the floating-point unit to report a comparison, and the first of those trips writes the number to memory and reads it straight back unchanged. The processor can do all of it in registers. Only that decision is replaced; unusual key types are still handed to the game's own code. The lookup only reads and returns a location, so it is checked against the game's own answer thirty thousand times and regularly afterwards.", true) },
                { "Line-of-Sight Box Test (SSE2)", new SettingItem("Graphics_Sound", "SegmentAabb", false, null, "Checking whether a line passes through a box. Almost all the time here goes on something other than the maths: to act on a comparison of two numbers, the old floating-point unit has to copy its status into a general register first, and the next instruction sits waiting for it. This function does that ten times per call, and the profiler's samples land exactly on the waiting instruction. Modern instructions produce the answer directly. Every one of those ten decisions was transcribed rather than guessed, including two that the game makes by inspecting raw bits rather than comparing values, so the answer is identical in every case. The test only reports a yes or no and changes nothing, so it is checked against the game's own answer twenty thousand times and regularly afterwards.", true) },
                { "Visibility Box Test (SSE2)", new SettingItem("Graphics_Sound", "FrustumAabb", false, null, "Deciding whether something is on screen means testing its bounding box against the six sides of the view. Most of what the game spends there is not the maths: for each side it checks the sign of a number, uses that to look up which corner of the box to use, and then fetches the corner through that lookup - eighteen times per test. The processor can pick the corner directly from those signs with no lookup at all. The maths is done at the same width the game uses and in the same order, so the answer is identical. The test only reports a yes or no and changes nothing, so it is simply checked against the game's own answer, twenty thousand times at first and regularly afterwards.", true) },
                { "Model Draw Order Key Cache", new SettingItem("Graphics_Sound", "M2SortKey", false, null, "Before drawing a model the game sorts its pieces into the right order, and the routine that decides which of two pieces comes first spends almost all its time chasing pointers through memory to look up a single number - five hops, each waiting on the one before it. In a profile of a tester's session this one routine was 2.44 percent of all the time the game spent working, ahead of every scripting entry. This remembers that number for the length of a single frame, which removes three of the five hops. The comparison itself is unchanged and does not alter anything, so the result is simply checked against the game's own answer, twenty thousand times at first and regularly afterwards.", true) },
                { "Model Draw Order Sort", new SettingItem("Graphics_Sound", "M2BatchSort", false, null, "Every frame the game sorts the solid pieces of all visible models into drawing order. Each time it compares two pieces it follows the same chain of pointers for both of them first, and a sort compares each piece many times, so the routine doing it was 4.09 percent of the time the game spent working in a tester's profile, the largest single entry. This reads what the comparison needs once per piece before the sort starts and lets the game's own sort run on that. The order it produces is the same; for the first 512 sorts, and regularly afterwards, every comparison is also made the game's way and checked, and the first difference switches this off for the session.", true) },
                { "Collision Polygon Clip (SSE2)", new SettingItem("Graphics_Sound", "CollisionPolyClip", false, null, "When the game works out what a line of sight or a camera movement runs into, it cuts each surface against the planes around it. Before cutting anything it measures every corner of the surface against the plane, and most of the time the answer is that the surface is entirely on one side and nothing needs cutting - but it pays for the whole measurement to find that out, one corner at a time on the old floating point stack. In a tester's profile this was 8.75 percent of the time the game spent working. This does the measurement with vector instructions at the same precision the game uses, answers the two simple cases, and hands anything that really has to be cut back to the game. The first twenty thousand calls are answered by the game and the decision checked against what it did, and the first disagreement switches this off for the session.", true) },
                { "Cloud Texture Reuse", new SettingItem("Graphics_Sound", "SkyTextureReuse", false, null, "The game draws its clouds from a texture it builds itself, a few rows every frame, cycling through the whole thing over and over. In a tester's profile that was 10.29 percent of the time the game spent working - the single largest thing it does. The pattern only moves when a counter tied to the cloud speed ticks over, so a full cycle that passes without a tick rebuilds exactly the bytes that are already there. This spots that and skips the rebuild, keeping everything else the game does. Whether it ever happens depends on the cloud speed and your frame rate, so it measures first: until it has seen rebuilds come out identical several times it changes nothing at all, and the log says how many passes could have been skipped.", true) },
                { "Particle Track Evaluation (SSE2)", new SettingItem("Graphics_Sound", "ParticleTrackEval", false, null, "Every particle in every effect has its colour, size and two more values worked out from how far through its life it is. The game does that on the old floating point stack, and each of the seven values goes out to memory and comes back before it is turned into a number it can use. In a tester's profile this one routine was 1.75 percent of the time the game spent working, and the particle update around it another 2.09. This does the same arithmetic with vector instructions at the same precision and with the same rounding. The first twenty thousand particles are worked out both ways and compared byte for byte, and the first difference switches this off for the session.", true) },
                { "Shader Constant Compare (SSE2)", new SettingItem("Graphics_Sound", "ShaderConstDedup", false, null, "Before the game sends numbers to the graphics card it checks each one against its own copy of what it sent last time, so it can skip the ones that have not changed. It does that check one number at a time on the old floating point stack, four per register, and in a tester's profile the checking was 1.17 percent of the time the game spent working. This compares all four at once with a single vector instruction. Nothing is calculated - the numbers are only compared and copied - so the result is the same bits for the same reason a copy is. The first four thousand calls are done by the game as well and the result compared byte for byte, and the first difference switches this off for the session.", true) },
                { "Batch Colour Convert", new SettingItem("Graphics_Sound", "BatchColourConvert", false, null, "Setting up each piece of a model to be drawn, the game turns three fractions into a colour. Turning a fraction into a whole number on the old floating point unit means switching its rounding mode and switching it back, twice per number, and each switch empties that unit's pipeline - in a tester's profile the single hottest instruction in the whole routine was one of those switches. This does the three conversions with one instruction each and no mode change. The game's own version of this code was copied instruction for instruction and run against this one over four million cases with no difference, six of those cases are re-checked when the game starts, and the twenty bytes around the code being replaced are compared first, so a different build of the game is refused rather than half-changed.", true) },
                { "Bone Movement Track (SSE2)", new SettingItem("Graphics_Sound", "AnimVec3Track", false, null, "Alongside a rotation, every animated bone carries a position, and the game works out where it should be by interpolating between two keyframes one number at a time. This does all three at once. It runs more often than the rotation work does - the same routine handles every three-number track in a model, and the animation code calls it eight times over against once for rotations. The tricky part is that the game rounds the result to lower precision in one place and deliberately does not in another, so both are reproduced exactly where they happen and the position that comes out is identical bit for bit, not merely close. It checks itself against the game's own answer for the first thirty thousand bones and keeps rechecking afterwards.", true) },
                { "Bone Rotation Unpack (SSE2)", new SettingItem("Graphics_Sound", "AnimQuatUnpack", false, null, "Posing a skeleton means reading a rotation for every bone of every animated thing on screen, every frame - and each rotation is stored packed into four small integers that have to be expanded back into real numbers. A 32-bit processor has no direct route from an integer to the old floating-point unit, so the game writes each number to memory and immediately reads it back again, four times per rotation and up to sixteen times per bone. This converts two at a time inside the processor with no memory in the way. The maths is done at the same width the game uses and rounded at the same points, so the pose that comes out is identical bit for bit, not merely close. It compares all its output against the game's own for the first thirty thousand bones and keeps rechecking afterwards.", true) },
                { "Spread Model Animation (crowd throttle)", new SettingItem("Graphics_Sound", "AnimLod", false, null, "Posing the skeletons of everything on screen is the single largest block of frame time the game spends: measured on real sessions at 3.68 milliseconds out of a 24.5 millisecond frame in a raid, across 114 models averaging 31 bones each. No one function inside it is worth optimising - the cost is spread across dozens - so the only way to reach it is to do less of it. Below 96 models on screen this changes nothing at all. Above that, each model has its pose refreshed every second, third or fourth frame instead of every frame, never less often than a quarter of your frame rate, and a model is never skipped before its first pose. It cannot make animations run slow or drift: the game works out where an animation should be from the clock each time rather than by counting frames, so a skipped update only delays when a pose is refreshed.\r\n\r\nReported stuttering visibly on environment animation, the Deeprun Tram tunnels among them. Its guard protects models whose materials or attachments still need work; it does not protect the skeleton, which is the thing being held. No measured frame gain stands against that yet - the sessions that showed the stutter were frame capped, where a saving inside the frame changes no frame time.", true) },
                { "UI Method Object Lookup", new SettingItem("UI_Lua", "LuaThisFast", false, null, "Every call an addon makes into a frame - SetText, GetWidth, Show, all 674 of them - starts by fetching the frame object out of a table slot, and the game does that through four separate script-engine calls plus a push and a pop. This reads it directly instead. The one thing those calls do besides fetch is carry addon ownership between values, which decides what is allowed to touch protected actions, and that is reproduced exactly rather than skipped. Anything out of the ordinary is handed straight back to the game. It compares the first 20000 lookups against the game's own answer and switches off if any of them differ.", true) },
                { "Hash Lookup Chains", new SettingItem("General", "ObjMgrFindFast", false, null, "The game looks things up by id constantly - creatures, spells, items, database rows - through one search routine the compiler copied into the client eleven times. Every copy re-reads the table header and recomputes where the next link lives for each step of the search, although none of it can change during one lookup. This works it out once instead, on the three copies that are actually used heavily (one of them has 184 call sites). Each runs alongside the game's own routine at first and compares every answer; a single difference switches that one off for the session and says so in the log.", true) },
                { "Vertex Colour Format Inline", new SettingItem("Graphics_Sound", "VertexFmtInline", false, null, "The game asks \"does this colour need its bytes swapped for my graphics card\" once for every single vertex it builds, in both the interface batcher and the particle system. The answer is a property of your graphics device and cannot change between two vertices. Those two functions were 5% of CPU time in a profile. This computes the answer in place instead of calling out for it, using the same fourteen bytes of machine code, so nothing else shifts. It checks the client byte for byte first and does nothing if it does not match. EXPERIMENTAL: it patches game code.", true) },
                { "Lua Memory Pool Search", new SettingItem("UI_Lua", "LuaMemPoolFast", false, null, "The Lua allocator keeps memory in chunks and searches them from the beginning every single time it needs a block. Chunks that filled up early stay full, so once the pool has grown, every allocation walks past all of them first. A profile counted 2.3 million allocations in six minutes and put this function second overall. This starts the search where the last one succeeded. It cannot miss a free block: if the shorter search finds nothing, the original runs unchanged. Also counts how far the search really goes, so the log says whether it was ever the problem.", true) },
                { "CPU Core Class Report", new SettingItem("General", "CpuTopology", true, null, "Reads whether your CPU has both performance and efficiency cores, and records which kind the game's frame loop actually runs on. Measurement only, one call per frame. On hybrid CPUs (Intel 12th gen and newer) Windows decides where to put a thread, and a game that sleeps every frame looks like a light load, which is what gets moved onto a slow core. This tells you whether that is happening to you.") },
                { "Keep Game on Performance Cores", new SettingItem("General", "PinMainThread", false, null, "Keeps the game's main thread off the efficiency cores of a hybrid CPU, and asks Windows not to power-throttle it. Almost everything this client does happens on that one thread, so a slow core costs most of a frame. Off by default: check the core class report above first, and only turn this on if it shows time spent on efficiency cores. Does nothing on a CPU where every core is the same.") },
                { "Addon CPU by Sampling", new SettingItem("UI_Lua", "LuaAddonProfile", false, null, "Answers \"which addon is costing me frames\" without the client's own script profiler. The sampling profiler already stops the main thread a thousand times a second; this reads which addon's Lua is on the call stack while it is stopped, so it adds nothing to the code being measured. The client's profiler instead counts every entry and exit of every script, which one reporter measured at 1-4 fps in a dungeon. Needs the Sampling Profiler switch on, and follows it when this key is absent. Reports share of Lua time, not milliseconds.", true) },
                { "Asynchronous Texture Loader", new SettingItem("Graphics_Sound", "AsyncTexLoader", false, null, "Asynchronously loads and decompresses BLP textures in background worker threads, hot-swapping them on frame boundaries to prevent stutters. Marked experimental because until this build the switch was inert - it was written to one section of the ini and read from another - so nobody has ever run this, and worker threads are where this project's freezes have come from. Try it if you want to help, not because you expect it to help.", true) },
                { "Texture Smart Unload Delay", new SettingItem("Graphics_Sound", "TextureUnloadDelay", false, null, "Holds a texture the engine has finished with for five seconds, in case it is wanted again before then. Do not turn this on. Two testers have now measured it: 100,664 held for 380 reuses (0.4%), and 795,117 held for 1,652 (0.2%). Everything else expired and was released anyway. In 3.18.0 a bug stopped it working after the first loading screen, so nobody paid for it; 3.18.1 fixed the bug and it began doing its job for real, which means a lock and a hash insert on every texture the engine releases - 795,117 of them in one session - to save 1,652 reloads. It now measures its own reuse rate and switches off below one percent, but the honest answer is that the idea does not pay.", true) },
                { "Mipmap Bias Governor", new SettingItem("Graphics_Sound", "MipBiasGovernor", false, null, "Adjusts mipmap texture bias dynamically based on virtual memory pressure to prevent allocation spikes. Marked experimental for the same reason as the texture loader above: this switch was inert until this build, so no log anywhere shows what it does.", true) },
                { "SIMD Matrix Vector Transforms", new SettingItem("Graphics_Sound", "SimdMatrixTransform", false, null, "Vectorizes 3D coordinate and matrix-vector calculations using SSE2 SIMD instructions to accelerate particle updates.") },
                { "Advanced Sound Channels Coalescer", new SettingItem("Graphics_Sound", "SoundCoalescer", false, null, "Coalesces rapid duplicated sound plays to prevent channel exhaustion under AOE spam.") },
                { "Overlapping Sound Volume Limiter", new SettingItem("Graphics_Sound", "SoundVolumeLimit", false, null, "Limits and clamps volume for overlapping duplicate sound effects to prevent clipping and audio driver lag.") },
                { "Terrain Height Cache", new SettingItem("Graphics_Sound", "TerrainHeightCache", false, null, "Caches terrain elevation queries within the frame to minimize CPU map collisions query time.") },
                { "Spell Visual Effects Culler", new SettingItem("Graphics_Sound", "SpellEffectCulling", false, null, "Dynamically scales down particle density and minor spell impact effects in large raids.") },
                { "Lua String Interning Fast Path", new SettingItem("UI_Lua", "LuaSNewLstrFast", false, null, "Intercepts luaS_newlstr (0x00856C80), which every Lua string in the game passes through, and looks the string up in the VM's string table itself. Experimental: on a miss the engine repeats the same work, and it is under investigation as a possible cause of corrupted addon names. Leave off unless testing. Skipped by Enable All while it is under investigation.", true) },
                { "Fast SSE2 Memory Clear (FastMemset)", new SettingItem("Graphics_Sound", "FastMemsetOpt", true, null, "SSE2 non-temporal memset replacement for large memory clears at 0x0040BB80.") },
                { "Fast Case-Insensitive String Compare", new SettingItem("UI_Lua", "FastStrnicmpOpt", true, null, "SSE2 ASCII case-insensitive string comparison replacement at 0x0076E780.") },
            };

            // Window Setup
            Text = "WoW-Optimize Launcher";
            // The background is scaled to the client area and covered with a
            // near-opaque wash, so the height is free to change.
            ClientSize = new Size(920, 700);
            StartPosition = FormStartPosition.CenterScreen;
            FormBorderStyle = FormBorderStyle.None;
            BackColor = DarkBg;
            ForeColor = Color.White;
            Font = new Font("Segoe UI", 9f);
            MaximizeBox = false;
            DoubleBuffered = true;
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);

            // Load background image
            LoadBackgroundImage();

            // Build GUI
            InitializeLayout();

            // Load Settings from INI
            LoadSettings();

            // Check for Updates
            CheckForUpdatesAsync();
        }

        private void LoadBackgroundImage() {
            string exeDir = AppDomain.CurrentDomain.BaseDirectory;
            string bgImagePath = Path.Combine(exeDir, "wotlk_background.jpg");

            if (File.Exists(bgImagePath)) {
                try {
                    backgroundImage = Image.FromFile(bgImagePath);
                    return;
                } catch {
                    // fall through to resource
                }
            }

            try {
                Assembly asm = Assembly.GetExecutingAssembly();
                Stream stream = asm.GetManifestResourceStream("wotlk_background.jpg");
                if (stream != null) {
                    backgroundImage = Image.FromStream(stream);
                }
            } catch {
                // fallback to solid color — backgroundImage stays null
            }
        }

        protected override void OnPaintBackground(PaintEventArgs e) {
            Graphics g = e.Graphics;

            if (backgroundImage != null) {
                // Draw background image scaled to fill (UniformToFill equivalent)
                float scaleX = (float)ClientSize.Width / backgroundImage.Width;
                float scaleY = (float)ClientSize.Height / backgroundImage.Height;
                float scale = Math.Max(scaleX, scaleY);
                int drawW = (int)(backgroundImage.Width * scale);
                int drawH = (int)(backgroundImage.Height * scale);
                int drawX = (ClientSize.Width - drawW) / 2;
                int drawY = (ClientSize.Height - drawH) / 2;
                g.DrawImage(backgroundImage, drawX, drawY, drawW, drawH);

                // Dark overlay (alpha ~235/255 of RGB 12,12,18)
                using (SolidBrush overlay = new SolidBrush(Color.FromArgb(235, 12, 12, 18))) {
                    g.FillRectangle(overlay, ClientRectangle);
                }
            } else {
                using (SolidBrush bgBrush = new SolidBrush(DarkerBg)) {
                    g.FillRectangle(bgBrush, ClientRectangle);
                }
            }

            // Outer border
            using (Pen borderPen = new Pen(SeparatorColor, 1f)) {
                g.DrawRectangle(borderPen, 0, 0, ClientSize.Width - 1, ClientSize.Height - 1);
            }
        }

        // ── Drag support ─────────────────────────────────────────
        protected override void OnMouseDown(MouseEventArgs e) {
            if (e.Button == MouseButtons.Left) {
                dragging = true;
                dragStart = new Point(e.X, e.Y);
            }
            base.OnMouseDown(e);
        }

        protected override void OnMouseMove(MouseEventArgs e) {
            if (dragging) {
                Point p = PointToScreen(e.Location);
                Location = new Point(p.X - dragStart.X, p.Y - dragStart.Y);
            }
            base.OnMouseMove(e);
        }

        protected override void OnMouseUp(MouseEventArgs e) {
            dragging = false;
            base.OnMouseUp(e);
        }

        // ── Tooltip owner-draw ───────────────────────────────────
        private void ToolTip_Popup(object sender, PopupEventArgs e) {
            string text = toolTip.GetToolTip(e.AssociatedControl);
            using (Graphics g = e.AssociatedControl.CreateGraphics()) {
                using (Font f = new Font("Segoe UI", 9f)) {
                    SizeF sz = g.MeasureString(text, f, 350);
                    e.ToolTipSize = new Size((int)Math.Ceiling(sz.Width) + 16, (int)Math.Ceiling(sz.Height) + 12);
                }
            }
        }

        private void ToolTip_Draw(object sender, DrawToolTipEventArgs e) {
            using (SolidBrush bgBrush = new SolidBrush(Color.FromArgb(20, 20, 30))) {
                e.Graphics.FillRectangle(bgBrush, e.Bounds);
            }
            using (Pen borderPen = new Pen(CyanAccent, 1f)) {
                e.Graphics.DrawRectangle(borderPen, 0, 0, e.Bounds.Width - 1, e.Bounds.Height - 1);
            }
            using (SolidBrush textBrush = new SolidBrush(Color.White)) {
                using (Font f = new Font("Segoe UI", 9f)) {
                    e.Graphics.DrawString(e.ToolTipText, f, textBrush, new RectangleF(8, 6, e.Bounds.Width - 16, e.Bounds.Height - 12));
                }
            }
        }

        // ── Layout ───────────────────────────────────────────────
        private void InitializeLayout() {
            SuspendLayout();

            // ── LEFT PANEL ──────────────────────────────────────
            DoubleBufferedPanel leftPanel = new DoubleBufferedPanel();
            leftPanel.Location = new Point(10, 10);
            leftPanel.Size = new Size(280, ClientSize.Height - 20);
            leftPanel.BackColor = Color.Transparent;
            leftPanel.AutoScroll = false;

            int y = 10;

            // Title
            Label headerLabel = new Label();
            headerLabel.Text = "WOW OPTIMIZE";
            headerLabel.Font = new Font("Segoe UI", 18f, FontStyle.Bold);
            headerLabel.ForeColor = CyanAccent;
            headerLabel.AutoSize = true;
            headerLabel.Location = new Point(15, y);
            headerLabel.BackColor = Color.Transparent;
            leftPanel.Controls.Add(headerLabel);
            y += headerLabel.PreferredHeight + 0;

            // Dev subtitle
            Label devLabel = new Label();
            devLabel.Text = "by Suprematist";
            devLabel.Font = new Font("Segoe UI", 8.5f, FontStyle.Italic);
            devLabel.ForeColor = CyanAccent;
            devLabel.AutoSize = true;
            devLabel.Location = new Point(17, y);
            devLabel.BackColor = Color.Transparent;
            leftPanel.Controls.Add(devLabel);
            y += devLabel.PreferredHeight + 2;

            // Subheader
            Label subHeaderLabel = new Label();
            subHeaderLabel.Text = "MOD CONFIGURATOR & LAUNCHER";
            // Same reason the tab strip needed NoPrefix: a Label eats & as a
            // mnemonic and underlines what follows, so this read CONFIGURATOR
            // with a gap where the ampersand should be.
            subHeaderLabel.UseMnemonic = false;
            subHeaderLabel.Font = new Font("Segoe UI", 7.5f, FontStyle.Regular);
            subHeaderLabel.ForeColor = SubHeaderColor;
            subHeaderLabel.AutoSize = true;
            subHeaderLabel.Location = new Point(17, y);
            subHeaderLabel.BackColor = Color.Transparent;
            leftPanel.Controls.Add(subHeaderLabel);
            y += subHeaderLabel.PreferredHeight + 18;

            // ── Actions ────────────────────────────────────────
            //
            // Three that set every switch at once, three that move a whole
            // configuration around, and Launch pinned to the bottom of the
            // column so it is in the same place whatever else is above it.
            //
            // What is not here: the two buttons that set up a measuring
            // session. They put a profiler on the main thread and an A/B
            // harness that flips eighteen features every twenty seconds, and
            // sitting in the same column as MAX PERFORMANCE they read like
            // something a player should press. Every switch they touched is
            // still a checkbox on the right.
            int btnWidth = 248;

            y += AddSectionLabel(leftPanel, "SET EVERYTHING AT ONCE", y);

            DarkButton btnMaxPerf = new DarkButton(Color.FromArgb(255, 170, 0), false);
            btnMaxPerf.Text = "MAX PERFORMANCE";
            btnMaxPerf.Size = new Size(btnWidth, 32);
            btnMaxPerf.Location = new Point(15, y);
            btnMaxPerf.Click += delegate { SetUpMaxPerformance(); };
            toolTip.SetToolTip(btnMaxPerf,
                "Everything that makes the game faster, on. Everything that only "
                + "measures it, off - a census or a profiler costs frames to produce "
                + "a number.\r\n\r\n"
                + "Also left off: the ones that buy frames by changing how the game "
                + "looks or sounds, and the handful that were measured and lost. "
                + "Each of those is still yours to tick; hover it to read what was "
                + "measured.");
            leftPanel.Controls.Add(btnMaxPerf);
            y += 38;

            DarkButton btnDefaults = new DarkButton(Color.FromArgb(120, 132, 160), false);
            btnDefaults.Text = "DEFAULT";
            btnDefaults.Size = new Size(btnWidth, 32);
            btnDefaults.Location = new Point(15, y);
            btnDefaults.Click += delegate { RestoreDefaults(); };
            toolTip.SetToolTip(btnDefaults,
                "Back to what a fresh install runs: the features that are on for "
                + "everyone, and nothing else.");
            leftPanel.Controls.Add(btnDefaults);
            y += 38;

            DarkButton btnVanilla = new DarkButton(Color.FromArgb(255, 23, 68), false);
            btnVanilla.Text = "EVERYTHING OFF";
            btnVanilla.Size = new Size(btnWidth, 32);
            btnVanilla.Location = new Point(15, y);
            btnVanilla.Click += delegate { TurnEverythingOff(); };
            toolTip.SetToolTip(btnVanilla,
                "The game as it ships, with the DLL loaded and doing nothing.\r\n\r\n"
                + "This is the first thing to try when something is wrong. If the "
                + "problem is still there with everything off, it is not us.");
            leftPanel.Controls.Add(btnVanilla);
            y += 38;

            DarkButton btnProve = new DarkButton(Color.FromArgb(0, 200, 140), false);
            btnProve.Text = "TRY THE UNPROVEN ONES";
            btnProve.Size = new Size(btnWidth, 32);
            btnProve.Location = new Point(15, y);
            btnProve.Click += delegate { SetUpProvingRun(); };
            toolTip.SetToolTip(btnProve,
                "Everything MAX PERFORMANCE leaves on, plus every replacement "
                + "marked [!] - the ones nobody has run in a game yet - and the "
                + "sampling profiler.\r\n\r\n"
                + "Each of those replacements checks its own answers against the "
                + "game's for thousands of calls before it answers anything, keeps "
                + "checking one call in a few thousand after that, and switches "
                + "itself off for the session at the first disagreement. The log "
                + "says which armed, which retired and why.\r\n\r\n"
                + "This is the session that decides whether they ship on. Play "
                + "normally for half an hour or more - a city, some combat - then "
                + "send Logs\\wow_optimize.log. Press MAX PERFORMANCE or DEFAULT "
                + "afterwards to put it back.");
            leftPanel.Controls.Add(btnProve);
            y += 40;

            y += AddSectionLabel(leftPanel, "WHEN SOMETHING IS WRONG", y);

            btnLogging = new DarkButton(Color.FromArgb(160, 120, 220), false);
            btnLogging.Size = new Size(btnWidth, 32);
            btnLogging.Location = new Point(15, y);
            btnLogging.Click += delegate { ToggleFullLogging(); };
            toolTip.SetToolTip(btnLogging,
                "Turn this on, play until the thing goes wrong, then send "
                + "Logs\\wow_optimize.log.\r\n\r\n"
                + "It switches on the sampling profiler and the counters that say "
                + "what the game was doing: where the main thread was, what was "
                + "drawn, what was compiled, what the shadow state looked like. "
                + "They cost frames, and that is the trade for a log that can "
                + "answer a question.\r\n\r\n"
                + "MAX PERFORMANCE and DEFAULT turn them all back off.");
            leftPanel.Controls.Add(btnLogging);
            y += 40;

            y += AddSectionLabel(leftPanel, "MOVE A CONFIGURATION", y);

            DarkButton btnSaveProfile = new DarkButton(CyanAccent, false);
            btnSaveProfile.Text = "SAVE TO FILE...";
            btnSaveProfile.Size = new Size(btnWidth, 30);
            btnSaveProfile.Location = new Point(15, y);
            btnSaveProfile.Click += delegate { SaveProfile(); };
            toolTip.SetToolTip(btnSaveProfile, "Write every switch to an ini you can keep or send.");
            leftPanel.Controls.Add(btnSaveProfile);
            y += 36;

            DarkButton btnLoadProfile = new DarkButton(CyanAccent, false);
            btnLoadProfile.Text = "LOAD FROM FILE...";
            btnLoadProfile.Size = new Size(btnWidth, 30);
            btnLoadProfile.Location = new Point(15, y);
            btnLoadProfile.Click += delegate { LoadProfile(); };
            toolTip.SetToolTip(btnLoadProfile, "Read a saved ini back in. Nothing is written until you launch.");
            leftPanel.Controls.Add(btnLoadProfile);
            y += 36;

            DarkButton btnShareProfile = new DarkButton(Color.FromArgb(255, 179, 0), false);
            btnShareProfile.Text = "COPY FOR THE DEV";
            btnShareProfile.Size = new Size(btnWidth, 30);
            btnShareProfile.Location = new Point(15, y);
            btnShareProfile.Click += delegate { ShareProfileWithDev(); };
            toolTip.SetToolTip(btnShareProfile,
                "Puts every switch on the clipboard as ini text. Paste it with a bug "
                + "report and the log so the two can be read together.");
            leftPanel.Controls.Add(btnShareProfile);
            y += 36;

            // ── Separator ───────────────────────────────────────
            DoubleBufferedPanel separator = new DoubleBufferedPanel();
            separator.Size = new Size(btnWidth, 1);
            separator.Location = new Point(15, y);
            separator.BackColor = SeparatorColor;
            leftPanel.Controls.Add(separator);
            y += 14;

            // ── DLL Status Card ─────────────────────────────────
            DoubleBufferedPanel statusCard = new DoubleBufferedPanel();
            statusCard.Size = new Size(btnWidth, 54);
            statusCard.Location = new Point(15, y);
            statusCard.BackColor = PanelBg;
            statusCard.BorderStyle = BorderStyle.None;
            statusCard.Paint += delegate(object sender, PaintEventArgs pe) {
                using (Pen bp = new Pen(Color.FromArgb(35, 35, 50), 1f)) {
                    pe.Graphics.DrawRectangle(bp, 0, 0, statusCard.Width - 1, statusCard.Height - 1);
                }
            };

            Label statusTitle = new Label();
            statusTitle.Text = "MODULE STATUS:";
            statusTitle.Font = new Font("Segoe UI", 7.5f, FontStyle.Bold);
            statusTitle.ForeColor = Color.FromArgb(140, 140, 170);
            statusTitle.AutoSize = true;
            statusTitle.Location = new Point(10, 6);
            statusTitle.BackColor = Color.Transparent;
            statusCard.Controls.Add(statusTitle);

            // Say which file is missing and where it was looked for.
            //
            // This tested version.dll alone and, when it was absent, said
            // "NOT LOADED / MISSING DLLs". A user who has wow_optimize.dll but
            // not version.dll, or who runs this from anywhere other than the
            // folder holding WoW.exe, gets a message naming neither the file nor
            // the directory and has nothing to act on. That is what "the new one
            // doesn't detect the .dll" looks like from the other side.
            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            bool haveLoader = File.Exists(Path.Combine(baseDir, "version.dll"));
            bool havePayload = File.Exists(Path.Combine(baseDir, "wow_optimize.dll"));
            int proxyId = haveLoader
                ? ProxyIdentity(Path.Combine(baseDir, "version.dll"))
                : ProxyUnknown;
            bool foreignLoader = (proxyId == ProxyForeign);
            bool dllActive = haveLoader && havePayload && !foreignLoader;
            string missing;
            if (haveLoader && havePayload) missing = "";
            else if (!haveLoader && !havePayload) missing = "version.dll and wow_optimize.dll";
            else if (!haveLoader) missing = "version.dll";
            else missing = "wow_optimize.dll";

            // A compatibility setting on the client's executable stops the proxy
            // loading while both files sit exactly where they should, so it is
            // looked for once they do.
            string compatLayers = dllActive ? CompatLayersInFolder(baseDir) : null;

            string statusText;
            if (foreignLoader) statusText = "NOT LOADED - version.dll belongs to another mod";
            else if (!dllActive) statusText = "NOT LOADED - missing " + missing;
            else if (compatLayers != null) statusText = "MAY NOT LOAD - compatibility setting";
            else statusText = "OPTIMIZER ACTIVE (version.dll)";

            Label statusVal = new Label();
            statusVal.Text = statusText;
            statusVal.Font = new Font("Segoe UI", 8.5f, FontStyle.Bold);
            if (!dllActive) statusVal.ForeColor = Color.FromArgb(255, 82, 82);
            else if (compatLayers != null) statusVal.ForeColor = Color.FromArgb(255, 193, 7);
            else statusVal.ForeColor = Color.FromArgb(0, 230, 118);
            statusVal.AutoSize = true;
            statusVal.Location = new Point(10, 26);
            statusVal.BackColor = Color.Transparent;
            statusCard.Controls.Add(statusVal);

            // The second line carries whatever the first one cannot act on: the
            // folder that was searched when a file is missing, the collision
            // when version.dll is someone else's, the compatibility setting and
            // where to clear it, and otherwise the result the proxy recorded on
            // the last launch. Both files being present is not evidence that
            // the payload loaded.
            string detail;
            if (foreignLoader) {
                detail = "that version.dll has no optimizer loader in it - ReShade and other mods use the same filename";
            } else if (!dllActive) {
                detail = "looked in " + baseDir;
            } else if (compatLayers != null) {
                detail = compatLayers + " - untick \"Disable fullscreen optimizations\" in its Properties > Compatibility";
            } else {
                string lastRun = LastProxyResult(baseDir);
                detail = (lastRun != null && lastRun.StartsWith("ERROR")) ? "last launch: " + lastRun : null;
            }

            if (detail != null) {
                statusCard.Size = new Size(btnWidth, 72);
                Label statusWhere = new Label();
                statusWhere.Text = detail;
                statusWhere.Font = new Font("Segoe UI", 7f, FontStyle.Regular);
                statusWhere.ForeColor = Color.FromArgb(150, 163, 178);
                statusWhere.AutoSize = false;
                statusWhere.Size = new Size(btnWidth - 20, 28);
                statusWhere.Location = new Point(10, 42);
                statusWhere.BackColor = Color.Transparent;
                statusCard.Controls.Add(statusWhere);
            }

            leftPanel.Controls.Add(statusCard);
            y += (detail != null) ? 80 : 62;

            activeCountLabel = new Label();
            activeCountLabel.Font = new Font("Segoe UI", 8f, FontStyle.Regular);
            activeCountLabel.ForeColor = Color.FromArgb(150, 163, 178);
            activeCountLabel.AutoSize = true;
            activeCountLabel.Location = new Point(17, y);
            activeCountLabel.BackColor = Color.Transparent;
            activeCountLabel.Text = "Active modules: 0/" + settingsMap.Count.ToString();
            leftPanel.Controls.Add(activeCountLabel);
            y += 20;

            progressBarPanel = new DoubleBufferedPanel();
            progressBarPanel.Size = new Size(btnWidth, 4);
            progressBarPanel.Location = new Point(17, y);
            progressBarPanel.BackColor = Color.FromArgb(30, 30, 45);
            progressBarPanel.Paint += ProgressBar_Paint;
            leftPanel.Controls.Add(progressBarPanel);
            y += 18;

            // Where the log is, because a bug report is worth nothing without it
            // and nobody should have to be told the path twice.
            Label logHint = new Label();
            logHint.Text = "The log is Logs\\wow_optimize.log, and its report\r\nstarts with a list of what did not work.";
            logHint.Font = new Font("Segoe UI", 7.5f, FontStyle.Regular);
            logHint.ForeColor = Color.FromArgb(130, 142, 158);
            logHint.AutoSize = false;
            logHint.Size = new Size(btnWidth, 30);
            logHint.Location = new Point(17, y);
            logHint.BackColor = Color.Transparent;
            leftPanel.Controls.Add(logHint);

            // ── Launch, pinned to the bottom ────────────────────
            //
            // Anchored rather than flowed, so the column can gain or lose a
            // button above without Launch moving. It used to follow the flow
            // and ended up halfway up the panel with a void underneath.
            int bottom = leftPanel.Height - 10;

            DarkButton btnExit = new DarkButton(Color.FromArgb(80, 88, 110), false);
            btnExit.Text = "EXIT LAUNCHER";
            btnExit.Size = new Size(btnWidth, 30);
            btnExit.Location = new Point(15, bottom - 30);
            btnExit.Click += delegate { Application.Exit(); };
            leftPanel.Controls.Add(btnExit);

            DarkButton btnLaunch = new DarkButton(CyanAccent, true);
            btnLaunch.Text = "LAUNCH WOW";
            btnLaunch.Font = new Font("Segoe UI", 12f, FontStyle.Bold);
            btnLaunch.Size = new Size(btnWidth, 46);
            btnLaunch.Location = new Point(15, bottom - 30 - 8 - 46);
            btnLaunch.Click += delegate { LaunchWow(); };
            leftPanel.Controls.Add(btnLaunch);

            versionLabel = new Label();
            versionLabel.Text = "v" + APP_VERSION + "-Release";
            versionLabel.Font = new Font("Segoe UI", 7f, FontStyle.Regular);
            versionLabel.ForeColor = Color.FromArgb(90, 90, 110);
            versionLabel.AutoSize = true;
            versionLabel.Location = new Point(17, y);
            versionLabel.BackColor = Color.Transparent;
            leftPanel.Controls.Add(versionLabel);

            Controls.Add(leftPanel);

            // ── RIGHT PANEL ─────────────────────────────────────
            int rightX = 300;
            int rightW = ClientSize.Width - rightX - 10;

            // Tip label
            Label tipLabel = new Label();
            // Two lines. It was one line 20 pixels tall holding six marks and a
            // sentence, so it showed three of the marks and cut the third in half.
            // The headings inside a tab carry this. The marks come back
            // onto the rows only while a search has flattened the groups.
            tipLabel.Text = "[+] faster   [!] not proven   [=] fixes   [.] logging\r\n"
                          + "[?] diagnostics   [-] changes the look   [x] didn't help";
            tipLabel.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
            tipLabel.ForeColor = CyanAccent;
            tipLabel.AutoSize = false;
            tipLabel.Size = new Size(rightW - 270, 30);
            tipLabel.Location = new Point(rightX, 8);
            toolTip.SetToolTip(tipLabel,
                "[+] makes the game faster, and something measured it.\r\n"
                + "[!] should make it faster; not measured yet. Turn one on, "
                + "play, and the log says what it did.\r\n"
                + "[=] protects or repairs something; no speed claim.\r\n"
                + "[?] measures the game, and costs frames to produce the number.\r\n"
                + "[-] buys frames by changing how the game looks or sounds.\r\n"
                + "[x] was measured against the client and lost; hover it to read what.\r\n"
                + "[.] writes a log; negligible cost.\r\n\r\n"
                + "Hover any feature for what it does.");
            tipLabel.BackColor = Color.Transparent;
            Controls.Add(tipLabel);

            // Search Label
            Label searchLabel = new Label();
            searchLabel.Text = "Search:";
            searchLabel.Font = new Font("Segoe UI", 9f, FontStyle.Bold);
            searchLabel.ForeColor = Color.White;
            searchLabel.AutoSize = true;
            searchLabel.Location = new Point(rightX + rightW - 260, 15);
            searchLabel.BackColor = Color.Transparent;
            Controls.Add(searchLabel);

            // Search TextBox
            searchBox = new TextBox();
            searchBox.Font = new Font("Segoe UI", 9f, FontStyle.Regular);
            searchBox.BackColor = Color.FromArgb(20, 20, 30);
            searchBox.ForeColor = Color.White;
            searchBox.BorderStyle = BorderStyle.FixedSingle;
            searchBox.Location = new Point(rightX + rightW - 200, 12);
            searchBox.Size = new Size(190, 20);
            searchBox.TextChanged += delegate { FilterFeatures(searchBox.Text); };
            Controls.Add(searchBox);

            // TabControl
            tabs = new DarkTabControl();
            tabs.Location = new Point(rightX, 44);
            tabs.Size = new Size(rightW, ClientSize.Height - 59);
            tabs.SelectedIndexChanged += delegate {
                if (searchBox != null) {
                    FilterFeatures(searchBox.Text);
                }
            };

            // Create tab pages
            TabPage tpGeneral = CreateTabPage("GENERAL");
            TabPage tpUiLua = CreateTabPage("UI & LUA");
            TabPage tpCombatNet = CreateTabPage("COMBAT & NET");
            TabPage tpGraphicsSound = CreateTabPage("GRAPHICS & SOUND");
            TabPage tpTried = CreateTabPage("TRIED, DIDN'T HELP");
            TabPage tpDiag = CreateTabPage("DIAGNOSTICS");

            tabs.TabPages.Add(tpGeneral);
            tabs.TabPages.Add(tpUiLua);
            tabs.TabPages.Add(tpCombatNet);
            tabs.TabPages.Add(tpGraphicsSound);
            tabs.TabPages.Add(tpTried);
            tabs.TabPages.Add(tpDiag);

            // Get the scroll panels from each tab page
            generalFlow = (FlowLayoutPanel)((Panel)tpGeneral.Controls[0]).Controls[0];
            uiLuaFlow = (FlowLayoutPanel)((Panel)tpUiLua.Controls[0]).Controls[0];
            combatNetFlow = (FlowLayoutPanel)((Panel)tpCombatNet.Controls[0]).Controls[0];
            graphicsSoundFlow = (FlowLayoutPanel)((Panel)tpGraphicsSound.Controls[0]).Controls[0];
            triedFlow = (FlowLayoutPanel)((Panel)tpTried.Controls[0]).Controls[0];
            diagFlow = (FlowLayoutPanel)((Panel)tpDiag.Controls[0]).Controls[0];



            // One control per switch, made once. Where it goes and what it is
            // called are decided by Rebuild, which runs again on every search.
            foreach (KeyValuePair<string, SettingItem> pair in settingsMap) {
                SettingItem data = pair.Value;
                DarkCheckBox chk = CreateStyledCheckBox(pair.Key, data.Tooltip);
                data.Ctrl = chk;
                chk.CheckedChanged += delegate { UpdateActiveModulesCount(); };
            }
            Rebuild("");

            Controls.Add(tabs);
            ResumeLayout(false);
        }

        private void FilterFeatures(string query) {
            Rebuild(query);
        }

        // Fills the four tabs. Called once at start-up and again on every
        // keystroke in the search box.
        //
        // Without a search the rows are grouped by what the switch is for, with
        // a heading over each run. A hundred and twenty-three checkboxes in one
        // column is a wall, and reading it told you nothing about which of them
        // you would want. Grouped, a tab opens on the ones that make the game
        // faster and the profilers are at the bottom under a heading that says
        // they cost frames.
        //
        // The mark is on the row only while searching. A search flattens the
        // groups, so the row has to carry its own label again; under a heading
        // that already says MAKES THE GAME FASTER, a [+] in front of every line
        // is the same word twice.
        private void Rebuild(string query) {
            if (generalFlow == null || uiLuaFlow == null ||
                combatNetFlow == null || graphicsSoundFlow == null ||
                triedFlow == null || diagFlow == null) {
                return;
            }

            query = (query ?? "").Trim().ToLower();
            bool hasSearch = !string.IsNullOrEmpty(query);

            FlowLayoutPanel[] flows = new FlowLayoutPanel[] {
                generalFlow, uiLuaFlow, combatNetFlow, graphicsSoundFlow,
                triedFlow, diagFlow
            };
            // The checkboxes are made once and reused, so they are only removed.
            // The group headings are made fresh every time this runs, which is
            // every keystroke in the search box, so they have to be disposed or
            // they pile up for the life of the window.
            for (int i = 0; i < flows.Length; i++) {
                List<Control> headings = new List<Control>();
                foreach (Control c in flows[i].Controls) {
                    if (c is Label) headings.Add(c);
                }
                flows[i].Controls.Clear();
                for (int h = 0; h < headings.Count; h++) headings[h].Dispose();
            }

            foreach (KeyValuePair<string, SettingItem> pair in settingsMap) {
                if (pair.Value.Ctrl == null) continue;
                pair.Value.Ctrl.Visible =
                    !hasSearch || pair.Key.ToLower().Contains(query);
            }

            for (int f = 0; f < flows.Length; f++) {
                for (int k = 0; k < Kinds.Order.Length; k++) {
                    string kind = Kinds.Order[k];
                    bool headed = false;

                    foreach (KeyValuePair<string, SettingItem> pair in settingsMap) {
                        SettingItem data = pair.Value;
                        if (data.Ctrl == null || !data.Ctrl.Visible) continue;
                        if (FlowFor(data) != flows[f]) continue;
                        if (Kinds.Of(data.Key, data.Experimental) != kind) continue;

                        if (!headed && !hasSearch) {
                            flows[f].Controls.Add(MakeGroupHeader(Kinds.Heading(kind)));
                            headed = true;
                        }
                        data.Ctrl.Text = hasSearch ? (kind + " " + pair.Key) : pair.Key;
                        flows[f].Controls.Add(data.Ctrl);
                    }
                }
            }
        }

        private Label MakeGroupHeader(string text) {
            Label l = new Label();
            l.Text = text;
            l.Font = new Font("Segoe UI", 7.5f, FontStyle.Bold);
            l.ForeColor = Color.FromArgb(110, 122, 140);
            l.AutoSize = true;
            l.Margin = new Padding(6, 14, 5, 4);
            l.BackColor = Color.Transparent;
            return l;
        }

        private TabPage CreateTabPage(string title) {
            TabPage tp = new TabPage(title);
            tp.BackColor = DarkBg;
            tp.ForeColor = Color.White;
            tp.Padding = new Padding(0);

            // Scrollable container panel
            Panel scrollPanel = new Panel();
            scrollPanel.Dock = DockStyle.Fill;
            scrollPanel.AutoScroll = true;
            scrollPanel.BackColor = DarkBg;

            scrollPanel.Scroll += delegate(object sender, ScrollEventArgs e) {
                scrollPanel.Invalidate(true);
                scrollPanel.Update();
            };

            scrollPanel.MouseWheel += delegate(object sender, MouseEventArgs e) {
                scrollPanel.Invalidate(true);
                scrollPanel.Update();
            };

            DoubleBufferedFlowPanel flow = new DoubleBufferedFlowPanel();
            flow.FlowDirection = FlowDirection.TopDown;
            flow.WrapContents = false;
            flow.AutoSize = true;
            flow.AutoSizeMode = AutoSizeMode.GrowOnly;
            flow.BackColor = DarkBg;
            flow.Padding = new Padding(5, 10, 5, 10);
            flow.Width = tabs.Width - 40;

            scrollPanel.Controls.Add(flow);
            tp.Controls.Add(scrollPanel);
            return tp;
        }

        
        // Which tab a switch belongs on. Both the initial build and the search
        // filter route through here, because they are the two places that have
        // already drifted apart once and emptied a tab between them.
        //
        // There is no Experimental tab any more. It was a maturity axis wearing
        // a category tab's clothes, and it held nearly half the switches, so
        // the four real categories were half empty and nothing could be found
        // where its name said it would be. Maturity is a property of a switch,
        // not a place to keep it, so it is a mark on the row instead - [!] for
        // what should help and has not been proven.
        // What a switch is decides its tab before which part of the game it
        // touches does. The two kinds below are the ones every preset button
        // leaves off, and a tester who presses one and then looks for what is
        // still unticked has one place to look instead of four.
        private FlowLayoutPanel FlowFor(SettingItem data) {
            string kind = Kinds.Of(data.Key, data.Experimental);
            if (kind == Kinds.Lost) return triedFlow;
            if (kind == Kinds.Diag || kind == Kinds.Log) return diagFlow;
            switch (data.Section) {
                case "General":        return generalFlow;
                case "UI_Lua":         return uiLuaFlow;
                case "Combat_Net":     return combatNetFlow;
                case "Graphics_Sound": return graphicsSoundFlow;
            }
            return generalFlow;
        }

        // A quiet heading over a run of buttons. Returns the height it used so
        // the caller's running y stays the only place that knows the layout.
        private int AddSectionLabel(Control parent, string text, int y) {
            Label l = new Label();
            l.Text = text;
            l.Font = new Font("Segoe UI", 7.5f, FontStyle.Bold);
            l.ForeColor = Color.FromArgb(110, 122, 140);
            l.AutoSize = true;
            l.Location = new Point(17, y);
            l.BackColor = Color.Transparent;
            parent.Controls.Add(l);
            return l.PreferredHeight + 4;
        }

        // The switches that make a log able to answer a question, rather than
        // only say that something happened. Every one of them records; none of
        // them changes what the game does.
        //
        // Deliberately not in here: the A/B harness, which turns features on and
        // off underneath you and would make a bug come and go, and No Client
        // Patches, which removes the patches rather than describing them. Both
        // are diagnostics and neither belongs in "I have a bug, record it".
        private static readonly string[] FullLoggingKeys = new string[] {
            "SessionLogs", "FlightRecorder", "NetDiag", "CpuTopology",
            "SamplingProfiler", "AddonProfiler", "LuaCompileCensus",
            "AnimCensus", "DrawCensus", "ShadowStateProbe"
        };

        // SamplingProfiler is the one that costs the most and the one nothing
        // else turns on, so it is what the button reads its state from.
        private bool FullLoggingOn() {
            SettingItem anchor = FindByKey("SamplingProfiler");
            return anchor != null && anchor.Ctrl != null && anchor.Ctrl.Checked;
        }

        private void ToggleFullLogging() {
            bool turnOn = !FullLoggingOn();
            for (int i = 0; i < FullLoggingKeys.Length; i++) {
                SettingItem item = FindByKey(FullLoggingKeys[i]);
                if (item != null && item.Ctrl != null) item.Ctrl.Checked = turnOn;
            }
            UpdateActiveModulesCount();
            SaveSettings();
            UpdateLoggingButton();
        }

        private void UpdateLoggingButton() {
            if (btnLogging == null) return;
            btnLogging.Text = FullLoggingOn() ? "LOGGING: FULL" : "LOGGING: NORMAL";
        }

        // Every switch off. The one direction that is always safe, and the first
        // thing to try when something is wrong.
        private void TurnEverythingOff() {
            foreach (SettingItem item in settingsMap.Values) {
                if (item.Ctrl != null) item.Ctrl.Checked = false;
            }
            UpdateActiveModulesCount();
            SaveSettings();
        }

        private void SaveProfile() {
            SaveFileDialog sfd = new SaveFileDialog();
            sfd.Filter = "Configuration Profiles (*.ini)|*.ini";
            sfd.FileName = "wow_opt_profile.ini";
            sfd.Title = "Save Configuration Profile";
            if (sfd.ShowDialog() == DialogResult.OK) {
                SaveSettingsToPath(sfd.FileName);
                MessageBox.Show("Saved to:\n" + sfd.FileName, "Profile saved",
                                MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
        }

        private void LoadProfile() {
            OpenFileDialog ofd = new OpenFileDialog();
            ofd.Filter = "Configuration Profiles (*.ini)|*.ini";
            ofd.Title = "Load Configuration Profile";
            if (ofd.ShowDialog() == DialogResult.OK) {
                LoadSettingsFromPath(ofd.FileName);
                MessageBox.Show("Loaded from:\n" + ofd.FileName, "Profile loaded",
                                MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
        }

        // Straight to the clipboard as ini text, because a bug report needs the
        // configuration beside the log and retyping 123 switches is how the two
        // stop matching.
        private void ShareProfileWithDev() {
            try {
                StringBuilder sb = new StringBuilder();
                sb.AppendLine("; wow_optimize " + APP_VERSION + " - switches at the time of the report");
                sb.AppendLine();

                string[] order = new string[] { "General", "UI_Lua", "Combat_Net", "Graphics_Sound" };
                for (int s = 0; s < order.Length; s++) {
                    sb.AppendLine("[" + order[s] + "]");
                    foreach (SettingItem item in settingsMap.Values) {
                        if (item.Section != order[s]) continue;
                        sb.AppendLine(item.Key + "=" +
                                      ((item.Ctrl != null && item.Ctrl.Checked) ? "1" : "0"));
                    }
                    sb.AppendLine();
                }

                Clipboard.SetText(sb.ToString());
                MessageBox.Show(
                    "On the clipboard. Paste it with the log file - Logs" + "\\" +
                    "wow_optimize.log - so the switches and what happened can be read "
                    + "together.",
                    "Copied", MessageBoxButtons.OK, MessageBoxIcon.Information);
            } catch (Exception ex) {
                MessageBox.Show("Could not copy: " + ex.Message, "Error",
                                MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private DarkCheckBox CreateStyledCheckBox(string name, string tooltipText) {
            DarkCheckBox chk = new DarkCheckBox();
            chk.Text = name;
            toolTip.SetToolTip(chk, tooltipText);
            return chk;
        }

        private void ProgressBar_Paint(object sender, PaintEventArgs e) {
            if (settingsMap == null) return;
            int activeCount = 0;
            foreach (SettingItem item in settingsMap.Values) {
                if (item.Ctrl != null && item.Ctrl.Checked) {
                    activeCount++;
                }
            }
            int totalCount = settingsMap.Count;
            if (totalCount == 0) return;

            int fillWidth = (int)((float)activeCount / totalCount * progressBarPanel.Width);
            using (SolidBrush cyanBrush = new SolidBrush(CyanAccent)) {
                e.Graphics.FillRectangle(cyanBrush, 0, 0, fillWidth, progressBarPanel.Height);
            }
        }

        // ── Settings Logic ───────────────────────────────────────

        
        // The features the A/B harness can measure, by ini key.
        //
        // It can only alternate a feature that installed, and a feature installs
        // only if its own switch is on. On a default install that is two of the
        // sixteen below, so a tester who ticks A/B and plays for an hour gets a
        // report about two things. A tester gives this project about one session a
        // week; that is what this button is for.
        //
        // MatrixVectorSse2 belongs in the list precisely because it is known to be
        // slower than the code it replaces. If a run reports it as faster, the
        // measurement is what is wrong, and the DLL says so in the log itself.
        // Every name here must be one a module passes to AbTest::IsSubject,
        // or the run turns the feature on and the harness never alternates it.
        // SimdGeometry was in this list and registers no subject; M2MatrixSimd
        // registers one and was missing. The A/B report prints the names that
        // did register, so a log says which side drifted.
        
        // The counting questions, which are not the same session as the A/B run.
        //
        // Counters only. Draw Call Merging is deliberately not here even though
        // it prints a count: it changes what the renderer sends, and a build of
        // it put a tester's world on screen as smeared triangles. A button that
        // says it turns on counters does not turn that on.
        //
        // These answer "how much of X is there", not "is X faster". Several of
        // them cost something to measure - the draw census wraps the busiest call
        // in the renderer - so running them during an A/B test would move the very
        // frame times that test is comparing. Two buttons, two sessions.
        
        // Every switch that speeds the game up, on. Everything that measures it,
        // off. See Kinds.NotForSpeed for what else is left off and why.
        //
        // This exists because the button that turned everything on read exactly
        // like the answer to "make it as fast as possible" and was not. A tester
        // pressed it and got twelve profilers, a census on every draw call, and
        // an A/B harness rotating eighteen features every twenty seconds. That
        // button is gone; this one is what it was being mistaken for.
        private void SetUpMaxPerformance() {
            int on = 0, off = 0, left = 0;
            foreach (SettingItem item in settingsMap.Values) {
                if (item.Ctrl == null) continue;
                bool want;
                if (item.Experimental) {
                    // Its own default, whichever way that points. This button
                    // used to turn every one of these on - forty-eight of them,
                    // including replacements that are off because nobody has run
                    // them in a game yet. A switch that exists to be left off has
                    // to survive the button that turns everything on, and the six
                    // that are on by default have to survive it too, so neither is
                    // decided here.
                    want = item.DefaultVal;
                    left++;
                } else {
                    want = Kinds.HelpsSpeed(item.Key);
                }
                item.Ctrl.Checked = want;
                if (want) on++; else off++;
            }
            UpdateActiveModulesCount();
            SaveSettings();
            MessageBox.Show(
                on.ToString() + " features on, " + off.ToString() + " left off.\r\n\r\n"
                + "Off: everything that measures the game, everything that buys "
                + "frames by changing how it looks or sounds, and the few that "
                + "were measured against the client and lost.\r\n\r\n"
                + left.ToString() + " switch(es) marked [+] or [!] were left at "
                + "their own default, because an unproven replacement should not "
                + "be turned on by a button that says performance.\r\n\r\n"
                + "Saved. Launch when ready.",
                "Max Performance", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        
        
        
        
        
        // The session that turns an unproven replacement into a proven one.
        //
        // Every [!] switch is one that was written against the disassembly,
        // verified offline where the maths allows it, and never run in a game.
        // They stay off for everyone until a log says otherwise, and nothing in
        // this launcher asked for that log: MAX PERFORMANCE deliberately leaves
        // them at their own default, which is off, so pressing every button in
        // the tool still produced a session that measured none of them.
        //
        // The profiler comes with them, because a session that proves a
        // replacement safe and cannot say what the frame time was spent on
        // answers half the question. The censuses stay off - they cost frames
        // and would move the very numbers this run is for.
        private void SetUpProvingRun() {
            int unproven = 0, on = 0, off = 0;
            // Counted by reason, so the message can say what it left off and
            // where to find it rather than leaving a tester to hunt for the
            // boxes that are still unticked.
            int diagOff = 0, lostOff = 0, tradeOff = 0;
            foreach (SettingItem item in settingsMap.Values) {
                if (item.Ctrl == null) continue;
                bool want;
                if (item.Key == "SamplingProfiler") {
                    want = true;
                } else if (item.Key == "FrameLimiter") {
                    // A run that spends its frames waiting measures nothing: the
                    // profile fills with the wait and every share in it is a
                    // share of whatever is left. This one switch is the
                    // difference between a log that answers the question and a
                    // log that cannot.
                    want = false;
                } else if (item.Experimental) {
                    // Only the ones that are a replacement for something the
                    // client does. A census is experimental too and belongs off.
                    want = Kinds.IsReplacement(item.Key);
                    if (want) unproven++;
                } else {
                    want = Kinds.HelpsSpeed(item.Key);
                }
                item.Ctrl.Checked = want;
                if (want) {
                    on++;
                } else {
                    off++;
                    string kind = Kinds.Of(item.Key, item.Experimental);
                    if (kind == Kinds.Diag || kind == Kinds.Log) diagOff++;
                    else if (kind == Kinds.Lost) lostOff++;
                    else if (kind == Kinds.Trade) tradeOff++;
                }
            }
            UpdateActiveModulesCount();
            SaveSettings();
            MessageBox.Show(
                unproven.ToString() + " unproven replacement(s) on, " + on.ToString()
                + " features on in all.\r\n\r\n"
                + "Each one checks its answers against the game's before it answers "
                + "anything, and switches itself off at the first disagreement. "
                + "Nothing here changes how the game looks or sounds.\r\n\r\n"
                + "Left off on purpose: " + diagOff.ToString() + " under the "
                + "DIAGNOSTICS tab, which cost frames and would move the very numbers "
                + "this run is for; " + lostOff.ToString() + " under TRIED, DIDN'T HELP, "
                + "each measured against the game and beaten by it; " + tradeOff.ToString()
                + " that buy frames by changing how the game looks or sounds, which is "
                + "your call and not this button's; and the frame rate limiter, because a "
                + "capped session measures nothing.\r\n\r\n"
                + "Play normally for half an hour or more, then send "
                + "Logs\\wow_optimize.log. Press MAX PERFORMANCE or DEFAULT to put "
                + "it back.\r\n\r\nSaved. Launch when ready.",
                "Try the unproven ones", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        private void RestoreDefaults() {
            foreach (SettingItem item in settingsMap.Values) {
                if (item.Ctrl != null) {
                    item.Ctrl.Checked = item.DefaultVal;
                }
            }
        }

        private void LoadSettings() {
            iniPath = ResolveIniPath();   // re-resolve: the DLL may have migrated it into WTF since startup
            LoadSettingsFromPath(iniPath);
        }

        private void LoadSettingsFromPath(string path) {
            if (!File.Exists(path)) {
                RestoreDefaults();
                return;
            }

            try {
                string[] lines = File.ReadAllLines(path);
                Dictionary<string, string> currentSettings = new Dictionary<string, string>();

                foreach (string line in lines) {
                    string trimmed = line.Trim();
                    if (string.IsNullOrEmpty(trimmed) || trimmed.StartsWith(";") || trimmed.StartsWith("["))
                        continue;

                    string[] parts = trimmed.Split('=');
                    if (parts.Length == 2) {
                        currentSettings[parts[0].Trim()] = parts[1].Trim();
                    }
                }

                foreach (string name in new List<string>(settingsMap.Keys)) {
                    SettingItem data = settingsMap[name];
                    string val;
                    if (currentSettings.TryGetValue(data.Key, out val)) {
                        data.Ctrl.Checked = (val == "1" || val.ToLower() == "true");
                    } else {
                        data.Ctrl.Checked = data.DefaultVal;
                    }
                }
                ApplyInheritedDefaults(currentSettings);
                UpdateActiveModulesCount();
            } catch (Exception ex) {
                MessageBox.Show("Error loading config profile: " + ex.Message, "Load Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                RestoreDefaults();
            }
        }

        private void SaveSettings() {
            iniPath = ResolveIniPath();
            SaveSettingsToPath(iniPath);
        }

        // Switches split out of larger ones read their old parent as their default
        // while their own key is absent, so that an existing wow_opt.ini keeps the
        // behaviour it had. The DLL does this in Config::Load. The launcher has to
        // resolve it the same way, because it writes every key on save: a config
        // that had DbcLookupCache=1 and no FileIoHooks line would otherwise get
        // FileIoHooks=0 written into it the first time anyone pressed Save, and the
        // file layer would go off without anyone asking for that.
        private SettingItem FindByKey(string key) {
            foreach (SettingItem item in settingsMap.Values) {
                if (item.Key == key) return item;
            }
            return null;
        }

        private void InheritIfAbsent(Dictionary<string, string> present, string key, string parentKey) {
            if (present.ContainsKey(key)) return;
            SettingItem child = FindByKey(key);
            SettingItem parent = FindByKey(parentKey);
            if (child == null || parent == null) return;
            if (child.Ctrl == null || parent.Ctrl == null) return;
            child.Ctrl.Checked = parent.Ctrl.Checked;
        }

        private void ApplyInheritedDefaults(Dictionary<string, string> present) {
            InheritIfAbsent(present, "FileIoHooks", "DbcLookupCache");
            InheritIfAbsent(present, "LuaTypeFast", "DbcLookupCache");
            InheritIfAbsent(present, "Win32ApiCaches", "TimingFix");
            InheritIfAbsent(present, "DebugApiHooks", "CvarNullGuard");
            InheritIfAbsent(present, "LockSpinHooks", "DefragLf");
            InheritIfAbsent(present, "AsyncWorkerPool", "DefragLf");
            InheritIfAbsent(present, "ThreadAffinity", "DefragLf");
            InheritIfAbsent(present, "SimdGeometry", "StrStrSse2");
            // Render state dedup used to be gated on EITHER of two switches, so
            // an absent key has to resolve to their OR, exactly as Config::Load
            // resolves it. InheritIfAbsent assigns rather than ORs, so calling it
            // twice would let the second parent switch off what the first
            // switched on, and the first Save would then take the feature away
            // from everyone who had it through the other one.
            if (!present.ContainsKey("RenderStateDedup")) {
                SettingItem dedup = FindByKey("RenderStateDedup");
                SettingItem dxvk  = FindByKey("VulkanDXVK");
                SettingItem rthr  = FindByKey("D3d9RenderThread");
                if (dedup != null && dedup.Ctrl != null) {
                    bool on = (dxvk != null && dxvk.Ctrl != null && dxvk.Ctrl.Checked)
                           || (rthr != null && rthr.Ctrl != null && rthr.Ctrl.Checked);
                    dedup.Ctrl.Checked = on;
                }
            }
            InheritIfAbsent(present, "LuaAddonProfile", "SamplingProfiler");
            // UiScriptHandlerCache and UnitApiFastPath used to inherit UIFrameBatch
            // here. Their checkboxes are gone because both gate an install that
            // can only return false, so there is nothing left to inherit and
            // InheritIfAbsent would find no control anyway. The DLL still reads
            // both keys and still inherits UIFrameBatch for them; that costs a
            // branch and turns on nothing.
        }

        private void SaveSettingsToPath(string path) {
            try {
                string dir = Path.GetDirectoryName(path);
                if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir)) {
                    Directory.CreateDirectory(dir);
                }

                Dictionary<string, List<string>> sections = new Dictionary<string, List<string>>() {
                    { "General", new List<string>() },
                    { "UI_Lua", new List<string>() },
                    { "Combat_Net", new List<string>() },
                    { "Graphics_Sound", new List<string>() }
                };

                foreach (SettingItem item in settingsMap.Values) {
                    string val = (item.Ctrl != null && item.Ctrl.Checked) ? "1" : "0";
                    sections[item.Section].Add(item.Key + "=" + val);
                }

                // Keys this launcher does not own, carried across.
                //
                // The writer below truncates the file and rebuilds it from the
                // table above, so every key without a tickbox here used to be
                // destroyed by pressing Save: thirteen boolean settings the DLL
                // reads, every numeric one - SleepPrecisionValue, SessionLogsToKeep,
                // FlightRecorderKey, AbTestPeriodMs - and AbTestSubject, which names
                // the feature an A/B run measures and whose own tooltip tells you to
                // set it by hand.
                //
                // A tester who edited one of those, opened this launcher and saved,
                // lost it without being told. Anything already in the file whose key
                // the launcher does not own is kept now, in the section it was found
                // in.
                try {
                    if (File.Exists(path)) {
                        string current = "General";
                        foreach (string raw in File.ReadAllLines(path)) {
                            string line = raw.Trim();
                            if (line.Length == 0 || line[0] == ';') continue;
                            if (line[0] == '[' && line[line.Length - 1] == ']') {
                                current = line.Substring(1, line.Length - 2);
                                continue;
                            }
                            int eq = line.IndexOf('=');
                            if (eq <= 0) continue;
                            string key = line.Substring(0, eq).Trim();
                            if (FindByKey(key) != null) continue;
                            if (!sections.ContainsKey(current)) continue;
                            sections[current].Add(key + "=" + line.Substring(eq + 1).Trim());
                        }
                    }
                } catch {
                    // An unreadable existing file must not stop the save. The owned
                    // keys are still written; only the carry-over is lost, which is
                    // what every save did before this.
                }

                using (StreamWriter sw = new StreamWriter(path, false, Encoding.UTF8)) {
                    sw.WriteLine("; WoW-Optimize Mod Configuration Profile");
                    sw.WriteLine("; Generated by Launcher");
                    sw.WriteLine();

                    foreach (KeyValuePair<string, List<string>> section in sections) {
                        sw.WriteLine("[" + section.Key + "]");
                        foreach (string line in section.Value) {
                            sw.WriteLine(line);
                        }
                        sw.WriteLine();
                    }
                }
            } catch (Exception ex) {
                MessageBox.Show("Error saving config profile: " + ex.Message, "Save Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        
        
        
        private void CheckForUpdatesAsync() {
            System.Threading.ThreadPool.QueueUserWorkItem(delegate {
                try {
                    // This launcher targets .NET Framework 4.0, whose default
                    // SecurityProtocol is SSL3 | TLS 1.0. GitHub stopped accepting
                    // both in 2018, so every request below failed at the handshake
                    // and the empty catch swallowed it - which is why the update
                    // notice has never once appeared for anyone.
                    //
                    // SecurityProtocolType.Tls12 does not exist as a named member
                    // in the 4.0 reference assemblies; 3072 is its value, and the
                    // runtime underneath is a later 4.x that understands it.
                    try {
                        System.Net.ServicePointManager.SecurityProtocol |=
                            (System.Net.SecurityProtocolType)3072;
                    } catch {
                        // Very old runtime with no TLS 1.2 at all: leave it alone
                        // and let the request fail as before.
                    }

                    using (System.Net.WebClient wc = new System.Net.WebClient()) {
                        wc.Headers.Add("User-Agent", "WoW-Optimize-Launcher");
                        string rawVer = wc.DownloadString("https://raw.githubusercontent.com/suprepupre/wow-optimize/main/version.txt?t=" + DateTime.UtcNow.Ticks.ToString());
                        if (!string.IsNullOrEmpty(rawVer)) {
                            string cleanVer = rawVer.Trim();
                            Version latest = new Version(cleanVer);
                            Version current = new Version(APP_VERSION);

                            if (latest > current) {
                                BeginInvoke(new Action(delegate { ShowUpdateAlert(cleanVer); }));
                            }
                        }
                    }
                } catch {
                    // Fail silently on network errors
                }
            });
        }

        private void ShowUpdateAlert(string latestVer) {
            if (versionLabel != null) {
                versionLabel.Text = "UPDATE AVAILABLE: v" + latestVer;
                versionLabel.ForeColor = Color.FromArgb(0, 230, 118);
                versionLabel.Font = new Font("Segoe UI", 7f, FontStyle.Bold);
                versionLabel.Cursor = Cursors.Hand;
                toolTip.SetToolTip(versionLabel, "Click to open GitHub releases page for upgrade!");
                versionLabel.Click += delegate {
                    try {
                        Process.Start("https://github.com/suprepupre/wow-optimize/releases");
                    } catch {
                        // ignore
                    }
                };
            }
        }

        private void LaunchWow() {
            // 1. Save Settings
            SaveSettings();

            // 2. Locate target executable
            string exeDir = AppDomain.CurrentDomain.BaseDirectory;
            string wowPath = Path.Combine(exeDir, "wow.exe");

            if (!File.Exists(wowPath)) {
                string[] alternateNames = { "Ascension.exe", "run.exe", "WoWCircle.exe", "wow-64.exe", "Sirus.exe" };
                foreach (string altName in alternateNames) {
                    string altPath = Path.Combine(exeDir, altName);
                    if (File.Exists(altPath)) {
                        wowPath = altPath;
                        break;
                    }
                }
            }

            // Fallback: search for any .exe containing "wow" or "ascension" that isn't the launcher/loader itself
            if (!File.Exists(wowPath)) {
                try {
                    string[] files = Directory.GetFiles(exeDir, "*.exe");
                    foreach (string file in files) {
                        string name = Path.GetFileName(file).ToLower();
                        if (name != "wow_optimize_launcher.exe" && name != "wow_loader.exe" && 
                            (name.Contains("wow") || name.Contains("ascension") || name.Contains("circle") || name.Contains("sirus"))) {
                            wowPath = file;
                            break;
                        }
                    }
                } catch {
                    // Ignore directory read errors
                }
            }

            if (!File.Exists(wowPath)) {
                MessageBox.Show("Could not find wow.exe, Ascension.exe, or another valid game executable in the current directory: " + exeDir + "\n\nPlease place the launcher in your World of Warcraft directory.", "Execution Error", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            try {
                ProcessStartInfo psi = new ProcessStartInfo();
                psi.FileName = wowPath;
                psi.WorkingDirectory = exeDir;
                Process.Start(psi);

                // Exit launcher on launch
                Close();
            } catch (Exception ex) {
                MessageBox.Show("Failed to launch " + Path.GetFileName(wowPath) + ": " + ex.Message, "Execution Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void UpdateActiveModulesCount() {
            // The logging button reads its state from the switches, so it
            // follows a preset and a hand-ticked box alike.
            UpdateLoggingButton();

            if (settingsMap == null) return;
            int activeCount = 0;
            foreach (SettingItem item in settingsMap.Values) {
                if (item.Ctrl != null && item.Ctrl.Checked) {
                    activeCount++;
                }
            }
            if (activeCountLabel != null) {
                activeCountLabel.Text = "Active modules: " + activeCount.ToString() + "/" + settingsMap.Count.ToString();
            }
            if (progressBarPanel != null) {
                progressBarPanel.Invalidate();
            }
        }
    }

    // ───────────────────────────────────────────────────────────────
    //  Application Entry Point
    // ───────────────────────────────────────────────────────────────
    public static class Program {
        [STAThread]
        public static void Main() {
            try {
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new MainForm());
            } catch (Exception ex) {
                try {
                    string crashPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "launcher_crash.txt");
                    File.WriteAllText(crashPath, ex.ToString());
                } catch {
                    // Ignore secondary logging failures
                }
                MessageBox.Show("Fatal launcher error:\n" + ex.Message + "\n\nDetails saved to launcher_crash.txt", "Fatal Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }
    }
}
