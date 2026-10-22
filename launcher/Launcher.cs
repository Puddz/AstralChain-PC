// Astral Chain PC launcher: install, compile shaders, play (with or without diagnostics) and settings, in one window.
// It drives the same Python steps and guard script as setup.cmd and "Play Astral Chain.cmd", without console windows.
// Build: python launcher/build.py (Windows' own C# 5 compiler; the exe needs only the .NET Framework 4 of Windows 10/11).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Management;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

namespace AstralChainLauncher
{
    static class Program
    {
        [System.Runtime.InteropServices.DllImport("user32.dll")]
        static extern bool SetProcessDPIAware();

        [STAThread]
        static void Main()
        {
            try { SetProcessDPIAware(); } catch { }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            string project = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
            if (!File.Exists(Path.Combine(project, @"astral_chain\setup_game.py")))
            {
                MessageBox.Show("Put this launcher in the Astral Chain PC folder (the one with setup.cmd and the " +
                    "astral_chain folder), then start it from there.", "Astral Chain PC", MessageBoxButtons.OK,
                    MessageBoxIcon.Warning);
                return;
            }
            Application.Run(new MainForm(project));
        }
    }

    // ------------------------------------------------------------------------------------------------ settings
    enum OptKind { Check, Choice, Number, Text }
    enum OptTarget { Game, Setup, Launcher }

    class Opt
    {
        public string Key, Group, Label, Help, Default;
        public OptKind Kind;
        public OptTarget Target;
        public string[] Choices;      // Choice: "value|label"
        public Func<string, string> Env; // value -> "NAME=value;..." ("" for none)
        public int Min, Max;
    }

    static class Options
    {
        public static readonly List<Opt> All = new List<Opt>();

        static Opt Add(string key, string group, string label, string help, OptKind kind, string def, OptTarget target,
            Func<string, string> env)
        {
            var o = new Opt { Key = key, Group = group, Label = label, Help = help, Kind = kind, Default = def,
                Target = target, Env = env };
            All.Add(o);
            return o;
        }

        static Func<string, string> When(string value, string env) { return v => v == value ? env : ""; }

        static Options()
        {
            Add("cutscene_fps_30", "Game", "Cutscenes at 30 fps, as on the Switch",
                "Off: in-game cutscenes run at 60 fps like the rest of the game.", OptKind.Check, "0",
                OptTarget.Game, When("1", "AC_CUTSCENE_FPS=30"));
            Add("ignore_focus", "Game", "Keep running while the game window is minimized",
                "Off: the game pauses while minimized, as on the Switch's home menu. Switching to another window " +
                "never pauses it; it only stops keyboard input.", OptKind.Check, "0",
                OptTarget.Game, When("1", "AC_IGNORE_FOCUS=1"));
            Add("save_snapshots", "Game", "Back up each save",
                "Every time the game saves, a copy goes next to the save in userdata\\saves (the newest 30 are kept).", OptKind.Check, "1",
                OptTarget.Game, When("0", "AC_SAVE_SNAPSHOTS=0"));
            Add("audio_resync", "Sound", "Skip sound gaps after a stall to stay in sync",
                "After a hitch, up to 0.3 s of sound is skipped so it does not fall behind the picture.",
                OptKind.Check, "1", OptTarget.Game, When("0", "AC_AUDIO_RESYNC=0"));
            Add("cut_carry", "Sound", "Keep cutscene cuts on time at 60 fps",
                "Off: cuts end as in the original code, each up to a frame early at 60 fps (sound drifts ahead).",
                OptKind.Check, "1", OptTarget.Game, When("0", "AC_CUT_CARRY=0"));
            var present = Add("present", "Display", "Presentation mode",
                "Mailbox: no tearing, the game paces itself (default). Vsync (FIFO): waits for the display, can slow " +
                "the game when the desktop is busy. Immediate: tearing, lowest latency.", OptKind.Choice, "mailbox",
                OptTarget.Game, v => v == "mailbox" ? "" : "LARECOMP_VK_PRESENT=" + v);
            present.Choices = new[] { "mailbox|Mailbox (default)", "fifo|Vsync (FIFO)", "immediate|Immediate" };
            Add("gpu_warm", "Shaders and performance", "Compile shaders for your GPU before playing",
                "Takes about 10 s the first time and after driver updates; removes most shader pauses in play.",
                OptKind.Check, "1", OptTarget.Setup, When("0", "AC_GPU_WARM=0"));
            var bc7 = Add("bc7", "Shaders and performance", "Texture compression (BC7)",
                "Saves about 2.5 GB of video memory (7.2 GB -> 4.7 GB on the highway chase), for GPUs with 8 GB or " +
                "less.\n" +
                "On the GPU: nothing to prepare and no disk space, but about 5 extra short pauses (60-100 ms) while " +
                "an area loads, and slightly less detail on some surfaces (bumps and reflections).\n" +
                "Prepared at setup: the best detail and about as smooth as off, but it takes about 12 minutes once " +
                "(at the next Play) and 16 GB of disk space.",
                OptKind.Choice, "off", OptTarget.Game, v => v == "gpu" || v == "setup" ? "LARECOMP_BC7=1" : "");
            bc7.Choices = new[] { "off|Off (default)", "gpu|On: converted on the GPU", "setup|On: prepared at setup (best)" };
            Add("gpu_astc", "Troubleshooting", "Decode ASTC textures on the GPU",
                "Off: textures are decoded on the CPU (slower loading). Try it if textures look wrong.",
                OptKind.Check, "1", OptTarget.Game, When("0", "LARECOMP_GPU_ASTC=0"));
            Add("gpl", "Troubleshooting", "Shader libraries (fast pipeline linking)",
                "Off: each pipeline is compiled whole when first drawn (longer pauses). Try it if graphics break.",
                OptKind.Check, "1", OptTarget.Game, When("0", "LARECOMP_GPL=0"));
            Add("pipe_warm", "Troubleshooting", "Build recorded pipelines ahead of their first draw",
                "Off: pipelines are only built when first drawn.", OptKind.Check, "1", OptTarget.Game,
                When("0", "LARECOMP_NO_PIPE_WARM=1"));
            var hang = Add("gpu_hang_ms", "Troubleshooting", "GPU hang stop (milliseconds)",
                "The game stops itself if the GPU stays busy this long, before Windows would reset the driver.",
                OptKind.Number, "1200", OptTarget.Game, v => v == "1200" ? "" : "AC_GPU_HANG_MS=" + v);
            hang.Min = 1000; hang.Max = 1800;
            Add("show_consoles", "Troubleshooting", "Show console windows",
                "Shows the game's console and the watchdog's window, as the .cmd launchers do.", OptKind.Check, "0",
                OptTarget.Launcher, null);
            Add("extra_env", "Troubleshooting", "Extra environment variables",
                "NAME=value;NAME=value, passed to the game (see docs\\development.md).", OptKind.Text, "",
                OptTarget.Game, v => v);
        }
    }

    class Settings
    {
        readonly string path;
        readonly Dictionary<string, string> values = new Dictionary<string, string>();

        public Settings(string path)
        {
            this.path = path;
            if (!File.Exists(path)) return;
            foreach (var line in File.ReadAllLines(path, Encoding.UTF8))
            {
                int eq = line.IndexOf('=');
                if (eq > 0 && !line.StartsWith("#")) values[line.Substring(0, eq).Trim()] = line.Substring(eq + 1);
            }
            string old;
            if (values.TryGetValue("bc7", out old) && old == "1") values["bc7"] = "gpu";  // (was a checkbox)
        }

        public string Get(string key)
        {
            string v;
            if (values.TryGetValue(key, out v)) return v;
            var o = Options.All.FirstOrDefault(x => x.Key == key);
            return o != null ? o.Default : "";
        }

        public string Raw(string key) { string v; return values.TryGetValue(key, out v) ? v : null; }
        public void Set(string key, string value) { values[key] = value; }
        public bool On(string key) { return Get(key) == "1"; }

        public void Save()
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            var lines = new List<string> { "# Astral Chain PC launcher settings" };
            lines.AddRange(values.OrderBy(kv => kv.Key).Select(kv => kv.Key + "=" + kv.Value));
            File.WriteAllLines(path, lines, new UTF8Encoding(false));
        }

        // "NAME=value;..." for one target
        public string Env(OptTarget target)
        {
            var parts = new List<string>();
            foreach (var o in Options.All)
            {
                if (o.Target != target || o.Env == null) continue;
                string value = Get(o.Key);
                if (o.Kind == OptKind.Number)
                {
                    int n;  // (a value edited out of range, or not a number, would reach the game as is)
                    if (!int.TryParse(value, out n)) n = int.Parse(o.Default);
                    value = Math.Max(o.Min, Math.Min(o.Max, n)).ToString();
                }
                string e = o.Env(value);
                if (!string.IsNullOrEmpty(e)) parts.Add(e.Trim().Trim(';'));
            }
            return string.Join(";", parts.Where(p => p.Length > 0));
        }
    }

    // ------------------------------------------------------------------------------------------------ main window
    class MainForm : Form
    {
        readonly string project, venvPython, logPath;
        readonly Settings settings;
        readonly NeonButton install, shaders, play, playDiag, settingsButton, controlsButton, cancel;
        readonly Label stateLabel, statusLabel;
        readonly NeonProgress progress;
        readonly TextBox log;
        readonly LinkLabel savesLink, logsLink, report, details;
        Image art;
        readonly string[] arts = Theme.ArtNames();
        int artIndex;
        Label artLabel;
        StreamWriter logFile;
        Thread worker;
        readonly object procLock = new object();
        Process current;
        bool cancelled, gameRunning;
        string shaderState = "", buildState = "";
        bool installed;

        protected override CreateParams CreateParams
        {
            get { var cp = base.CreateParams; cp.ClassStyle |= 0x20000; return cp; }  // (CS_DROPSHADOW)
        }

        public MainForm(string project)
        {
            this.project = project;
            venvPython = Path.Combine(project, @".venv\Scripts\python.exe");
            settings = new Settings(Path.Combine(project, @"userdata\launcher.ini"));
            logPath = Path.Combine(project, @"userdata\launcher.log");
            artIndex = Math.Max(0, Array.IndexOf(arts, settings.Raw("background")));
            art = arts.Length > 0 ? Theme.LoadArt(arts[artIndex]) : null;

            Text = "Astral Chain PC";
            Font = new Font("Segoe UI", 9.5F);
            ForeColor = Theme.Text;
            BackColor = Theme.Night;
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(1120, 630);
            DoubleBuffered = true;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            stateLabel = new Label { Text = "Checking...", AutoSize = false, Location = new Point(52, 150),
                Size = new Size(560, 22), ForeColor = Theme.Dim, BackColor = Color.Transparent };
            Controls.Add(stateLabel);

            int y = 190;
            play = MakeButton("PLAY", "60 fps, no console windows", y, 66, (s, e) => OnPlay(false));
            play.Primary = true; y += 80;
            playDiag = MakeButton("Play with diagnostics", "Logs slow frames and crashes for a bug report", y, 50,
                (s, e) => OnPlay(true)); y += 58;
            shaders = MakeButton("Compile shaders", "", y, 50, OnShaders); y += 58;
            install = MakeButton("Install game", "Your keys, base game and v1.0.1 update", y, 50, OnInstall); y += 58;
            settingsButton = MakeButton("Settings", "Game, sound and display", y, 50, OnSettings);
            settingsButton.Width = 186;
            controlsButton = MakeButton("Controls", "Keyboard keys", y, 50, OnControls);
            controlsButton.Bounds = new Rectangle(246, y, 186, 50);

            var close = new GlyphButton { Glyph = "\u2715", AccessibleName = "Close", Location = new Point(1076, 8), Size = new Size(36, 30) };
            close.Click += (s, e) => Close();
            var minimize = new GlyphButton { Glyph = "\u2014", AccessibleName = "Minimize", Location = new Point(1036, 8), Size = new Size(36, 30) };
            minimize.Click += (s, e) => WindowState = FormWindowState.Minimized;
            Controls.Add(close);
            Controls.Add(minimize);

            // Background picker (top right, under the window buttons); the choice is kept in launcher.ini
            if (arts.Length > 1)
            {
                var prev = new GlyphButton { Glyph = "\u2039", AccessibleName = "Previous background",
                    Location = new Point(980, 46), Size = new Size(34, 30), GlyphSize = 15f };
                var next = new GlyphButton { Glyph = "\u203A", AccessibleName = "Next background",
                    Location = new Point(1078, 46), Size = new Size(34, 30), GlyphSize = 15f };
                artLabel = new Label { AutoSize = false, Location = new Point(1014, 46), Size = new Size(64, 30),
                    TextAlign = ContentAlignment.MiddleCenter, ForeColor = Theme.Text, BackColor = Color.FromArgb(150, 6, 8, 18),
                    Font = new Font("Segoe UI Semibold", 9.5F) };
                var artTitle = new Label { Text = "BACKGROUND", AutoSize = false, Location = new Point(886, 46),
                    Size = new Size(94, 30), TextAlign = ContentAlignment.MiddleRight, ForeColor = Theme.Accent,
                    BackColor = Color.Transparent, Font = new Font("Segoe UI Semibold", 8.5F) };
                Controls.Add(artTitle);
                prev.Click += (s, e) => ShowArt(artIndex - 1);
                next.Click += (s, e) => ShowArt(artIndex + 1);
                Controls.Add(prev);
                Controls.Add(next);
                Controls.Add(artLabel);
                artLabel.Text = (artIndex + 1) + " / " + arts.Length;
            }

            statusLabel = new Label { AutoSize = false, Location = new Point(52, 560), Size = new Size(760, 22),
                ForeColor = Theme.Text, BackColor = Color.Transparent };
            progress = new NeonProgress { Location = new Point(52, 586), Size = new Size(904, 6) };
            cancel = new NeonButton { Text = "Cancel", Location = new Point(966, 566), Size = new Size(104, 34),
                Enabled = false, Visible = false };
            cancel.Click += OnCancel;
            Controls.Add(statusLabel);
            Controls.Add(progress);
            Controls.Add(cancel);

            savesLink = MakeLink("Saves folder", 636, (s, e) => OpenFolder(Path.Combine(project, @"userdata\saves")));
            logsLink = MakeLink("Logs folder", 738, (s, e) => OpenFolder(Path.Combine(project, @"build\game\round-00")));
            report = MakeLink("Create report", 832, (s, e) => CreateReport());
            details = MakeLink("Show log", 940, (s, e) => ShowLog(!log.Visible));

            log = new TextBox { Location = new Point(560, 300), Size = new Size(510, 240), Multiline = true,
                ReadOnly = true, ScrollBars = ScrollBars.Vertical, Font = new Font("Consolas", 8.5F),
                BackColor = Color.FromArgb(12, 14, 22), ForeColor = Color.FromArgb(190, 200, 220),
                BorderStyle = BorderStyle.FixedSingle, WordWrap = true, Visible = false };
            Controls.Add(log);

            MouseDown += DragWindow;
            stateLabel.MouseDown += DragWindow;
            statusLabel.MouseDown += DragWindow;

            AutoScaleDimensions = new SizeF(96F, 96F);
            AutoScaleMode = AutoScaleMode.Dpi;
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(logPath));
                logFile = new StreamWriter(logPath, false, new UTF8Encoding(false)) { AutoFlush = true };
            }
            catch { logFile = null; }
            Shown += (s, e) => RefreshState();
            FormClosing += OnClosing;
        }

        protected override void OnLoad(EventArgs e)
        {
            base.OnLoad(e);
            BackgroundImage = Theme.Backdrop(art, ClientSize, ClientSize.Width / 1120f);
        }

        void ShowArt(int index)
        {
            artIndex = (index + arts.Length) % arts.Length;
            var old = art;
            art = Theme.LoadArt(arts[artIndex]);
            var oldBackdrop = BackgroundImage;
            BackgroundImage = Theme.Backdrop(art, ClientSize, ClientSize.Width / 1120f);
            if (old != null) old.Dispose();
            if (oldBackdrop != null) oldBackdrop.Dispose();
            artLabel.Text = (artIndex + 1) + " / " + arts.Length;
            settings.Set("background", arts[artIndex]);
            try { settings.Save(); } catch { }
        }

        NeonButton MakeButton(string text, string sub, int y, int height, EventHandler click)
        {
            var b = new NeonButton { Text = text, Subtext = sub, Location = new Point(52, y), Size = new Size(380, height) };
            b.Click += click;
            Controls.Add(b);
            return b;
        }

        LinkLabel MakeLink(string text, int x, EventHandler click)
        {
            var l = new LinkLabel { Text = text, AutoSize = true, Location = new Point(x, 18), BackColor = Color.Transparent,
                LinkColor = Theme.Accent, ActiveLinkColor = Color.White, DisabledLinkColor = Theme.Dim,
                LinkBehavior = LinkBehavior.HoverUnderline };
            l.Click += click;
            Controls.Add(l);
            return l;
        }

        void ShowLog(bool show)
        {
            log.Visible = show;
            details.Text = show ? "Hide log" : "Show log";
        }

        void DragWindow(object sender, MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left) Theme.Drag(this);
        }

        // ---------------------------------------------------------------------------------------------- state
        void RefreshState()
        {
            stateLabel.Text = "Checking...";
            string message = statusLabel.Text;
            SetBusy(true, message, false);
            RunWorker(() =>
            {
                bool hasVenv = File.Exists(venvPython);
                string json = hasVenv ? CaptureOutput(venvPython, "-m astral_chain.setup_game --status", SetupEnv()) : "";
                Ui(() =>
                {
                    installed = hasVenv && Regex.IsMatch(json, "\"installed\":\\s*true");
                    var b = Regex.Match(json, "\"build\":\\s*\"(\\w+)\"");
                    var s = Regex.Match(json, "\"shaders\":\\s*\"(\\w+)\"");
                    buildState = b.Success ? b.Groups[1].Value : "missing";
                    shaderState = s.Success ? s.Groups[1].Value : "never";
                    ApplyState();
                    SetBusy(false, message, false);
                });
            });
        }

        void ApplyState()
        {
            install.Subtext = installed ? "(Installed)" : "Your keys, base game and v1.0.1 update";
            stateLabel.Text = installed
                ? "Installed in " + project + (buildState == "stale" ? "  (updated: rebuilds before the next start)" : "")
                : "Not installed yet. Start with Install game.";
            shaders.Subtext = !installed ? "Prepares every shader for your GPU" : shaderState == "compiled" ? "(Already compiled)"
                : shaderState == "needed" || buildState != "current" ? "(Needs recompiling)" : "(Not compiled yet)";
            UpdateButtons(false);
        }

        void UpdateButtons(bool busy)
        {
            install.Enabled = !busy && !installed;
            shaders.Enabled = !busy && installed;
            play.Enabled = !busy && installed;
            playDiag.Enabled = !busy && installed;
            settingsButton.Enabled = !busy;
            controlsButton.Enabled = !busy;
            report.Enabled = !busy && File.Exists(Path.Combine(project, @"build\game\run.txt"));
        }

        void SetBusy(bool busy, string status, bool cancellable)
        {
            UpdateButtons(busy);
            cancel.Enabled = busy && cancellable;
            cancel.Visible = cancel.Enabled;
            statusLabel.Text = status;
            progress.Style = busy ? ProgressBarStyle.Marquee : ProgressBarStyle.Blocks;
            if (!busy) progress.Value = 0;
        }

        // ---------------------------------------------------------------------------------------------- actions
        void OnInstall(object sender, EventArgs e)
        {
            string keys = PickFile("Select your prod.keys", "Keys (*.keys)|*.keys|All files|*.*", "last_keys");
            if (keys == null) return;
            string game = PickFile("Select the Astral Chain base game (XCI or NSP)",
                "Switch games (*.xci;*.nsp)|*.xci;*.nsp|All files|*.*", "last_base");
            if (game == null) return;
            string update = PickFile("Select the Astral Chain v1.0.1 update (NSP)",
                "Switch updates (*.nsp)|*.nsp|All files|*.*", "last_update");
            if (update == null) return;
            settings.Save();
            log.Clear();
            SetBusy(true, "Installing...", true);
            RunWorker(() =>
            {
                bool ok = EnsurePython() &&
                    Step("Installing the Python packages", venvPython,
                        "-m pip install --disable-pip-version-check -q -r requirements.txt", null) &&
                    Step("Installing the game", venvPython, "-u -m astral_chain.setup_game --keys " + Quote(keys) +
                        " --base " + Quote(game) + " --update " + Quote(update), SetupEnv());
                Finish(ok ? "Installed. Press Play to start the game." : null, "Install");
            });
        }

        void OnShaders(object sender, EventArgs e)
        {
            bool force = false;
            if (shaderState == "compiled" && buildState == "current")
            {
                if (MessageBox.Show(this, "The shaders are already compiled for this game build and your GPU.\n\n" +
                    "Check every shader and compile for your GPU again anyway?", "Compile shaders",
                    MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
                force = true;
            }
            log.Clear();
            SetBusy(true, "Compiling shaders...", true);
            RunWorker(() =>
            {
                bool ok = Step("Compiling shaders", venvPython, "-u -m astral_chain.setup_game --rebuild-if-stale" +
                    (force ? " --force-shaders" : ""), SetupEnv());
                Finish(ok ? "Shaders compiled." : null, "Compiling shaders");
            });
        }

        void OnPlay(bool diagnostics)
        {
            log.Clear();
            SetBusy(true, "Getting ready...", true);
            RunWorker(() =>
            {
                if (!Step("Getting ready", venvPython, "-u -m astral_chain.setup_game --rebuild-if-stale", SetupEnv()))
                {
                    Finish(null, "Getting the game ready");
                    return;
                }
                Ui(() => { statusLabel.Text = diagnostics ? "Playing (diagnostics)..." : "Playing...";
                    progress.Style = ProgressBarStyle.Marquee; cancel.Enabled = false; cancel.Visible = false; });
                string run = Path.Combine(project, @"build\game");
                string dump = Path.Combine(run, "crash.dmp");
                if (diagnostics) try { File.Delete(dump); } catch { }
                string env = "AC_PARALLEL=1;AC_IGNORE_FOCUS=;" + ControlsForm.Env(settings) + ";" + settings.Env(OptTarget.Game) +
                    (diagnostics ? ";LARECOMP_VK_PROF=1;AC_CRASH_DUMP=1" : "");
                bool consoles = settings.On("show_consoles");
                string args = "-NoProfile -ExecutionPolicy Bypass -File " +
                    Quote(Path.Combine(project, @"astral_chain\run_guarded.ps1")) + " -Run " + Quote(run) +
                    " -Seconds 604800 -Env " + Quote(env.Replace("\"", "")) + (consoles ? "" : " -NoConsole");
                gameRunning = true;
                Ui(() => WindowState = FormWindowState.Minimized);
                int code = RunProcess("powershell.exe", args, null, !consoles);
                gameRunning = false;
                Ui(() => { if (WindowState == FormWindowState.Minimized) WindowState = FormWindowState.Normal;
                    Activate(); });
                string why = ExitReason(code);
                Ui(() =>
                {
                    SetBusy(false, why ?? "The game was closed.", false);
                    RefreshState();
                    if (why != null || diagnostics)
                    {
                        string q = (why ?? "The game was closed.") + "\n\nCreate a report (logs" +
                            (File.Exists(dump) ? ", crash dump" : "") + " and system info, no game files) to attach " +
                            "to a bug report?";
                        if (MessageBox.Show(this, q, "Astral Chain PC", MessageBoxButtons.YesNo,
                            why != null ? MessageBoxIcon.Warning : MessageBoxIcon.Information) == DialogResult.Yes)
                            CreateReport();
                    }
                });
            });
        }

        static string ExitReason(int code)
        {
            switch (code)
            {
                case 0: return null;
                case 90: return "The game was closed by the driver watchdog: Windows logged a GPU driver error.";
                case 91: return "The game reached the time limit.";
                case 20: return "The game stopped at something not supported yet (STOP line at the end of trace.log).";
                case 32: return "The game crashed (FAULT line in stderr.log).";
                case -1: return "The game could not be started.";
                default: return "The game closed unexpectedly (exit code " + code + ").";
            }
        }

        void OnSettings(object sender, EventArgs e)
        {
            string oldBc7 = settings.Get("bc7");
            using (var f = new SettingsForm(settings))
            {
                if (f.ShowDialog(this) == DialogResult.OK)
                {
                    settings.Save();
                    BC7Changed(oldBc7, settings.Get("bc7"));
                    RefreshState();
                }
            }
        }

        // Texture compression switched to or away from "prepared at setup": say what happens next / offer the disk space back
        void BC7Changed(string before, string after)
        {
            string prepared = Path.Combine(project, @"build\game\cache\bc7");
            if (after == "setup" && before != "setup" && !File.Exists(Path.Combine(prepared, "complete.txt")))
                MessageBox.Show(this, "The textures are prepared the next time you press Play (or Compile shaders): " +
                    "about 12 minutes, once, and 16 GB of disk space.\n\nUntil then they are converted on the GPU.",
                    "Texture compression", MessageBoxButtons.OK, MessageBoxIcon.Information);
            if (before == "setup" && after != "setup" && Directory.Exists(prepared) &&
                MessageBox.Show(this, "Delete the prepared textures to free about 16 GB of disk space?\n\nChoosing " +
                    "\"prepared at setup\" again prepares them again (about 12 minutes).", "Texture compression",
                    MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
            {
                try { Directory.Delete(prepared, true); }
                catch (Exception ex) { Log("Could not delete " + prepared + ": " + ex.Message); }
            }
        }

        void OnControls(object sender, EventArgs e)
        {
            using (var f = new ControlsForm(settings))
            {
                if (f.ShowDialog(this) == DialogResult.OK)
                {
                    try { settings.Save(); } catch { }
                }
            }
        }

        void OnCancel(object sender, EventArgs e)
        {
            if (MessageBox.Show(this, "Stop the current step? It can be started again later; finished parts are kept.",
                "Cancel", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
            cancelled = true;
            KillCurrent();
        }

        void OnClosing(object sender, FormClosingEventArgs e)
        {
            if (worker == null || !worker.IsAlive) return;
            if (gameRunning)
            {
                if (MessageBox.Show(this, "The game is running. Close the launcher? The game keeps running.",
                    "Astral Chain PC", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                    e.Cancel = true;
                return;
            }
            if (MessageBox.Show(this, "A step is still running. Stop it and close?", "Astral Chain PC",
                MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) { e.Cancel = true; return; }
            cancelled = true;
            KillCurrent();
        }

        void Finish(string success, string what)
        {
            Ui(() =>
            {
                string msg = success ?? (cancelled ? what + " was cancelled." : what + " failed: see the messages below.");
                SetBusy(false, msg, false);
                cancelled = false;
                RefreshState();
                if (success == null && !msg.EndsWith("cancelled.")) ShowLog(true);
                if (success == null && !msg.EndsWith("cancelled."))
                    MessageBox.Show(this, msg, "Astral Chain PC", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            });
        }

        string PickFile(string title, string filter, string key)
        {
            using (var d = new OpenFileDialog { Title = title, Filter = filter, CheckFileExists = true })
            {
                string last = settings.Raw(key);
                if (!string.IsNullOrEmpty(last))
                {
                    try { d.InitialDirectory = Path.GetDirectoryName(last); d.FileName = Path.GetFileName(last); }
                    catch { }
                }
                if (d.ShowDialog(this) != DialogResult.OK) return null;
                settings.Set(key, d.FileName);
                return d.FileName;
            }
        }

        // ---------------------------------------------------------------------------------------------- python
        // Setup's own Python (tools\python, downloaded once) and .venv: astral_chain\get_python.ps1, as setup.cmd
        bool EnsurePython()
        {
            return Step("Getting Python", "powershell.exe",
                "-NoProfile -ExecutionPolicy Bypass -File " + Quote(Path.Combine(project, @"astral_chain\get_python.ps1")),
                null);
        }

        Dictionary<string, string> SetupEnv()
        {
            var env = new Dictionary<string, string>();
            foreach (var pair in settings.Env(OptTarget.Setup).Split(';'))
            {
                int eq = pair.IndexOf('=');
                if (eq > 0) env[pair.Substring(0, eq)] = pair.Substring(eq + 1);
            }
            // The game's settings change what the GPU warm-up compiles (shader libraries on or off)
            if (settings.Get("gpl") == "0") env["LARECOMP_GPL"] = "0";
            // BC7 prepared at setup: the next setup run (Play, Compile shaders) encodes the textures once
            if (settings.Get("bc7") == "setup") env["AC_BC7_PRECOMPUTE"] = "1";
            return env;
        }

        bool Step(string title, string exe, string args, Dictionary<string, string> env)
        {
            if (cancelled) return false;
            Ui(() => statusLabel.Text = title + "...");
            int code = RunProcess(exe, args, env, true);
            if (code != 0 && !cancelled) Log("(" + title + " stopped with exit code " + code + ")");
            return code == 0 && !cancelled;
        }

        // ---------------------------------------------------------------------------------------------- processes
        static ProcessStartInfo StartInfo(string exe, string args, Dictionary<string, string> env, bool hidden,
            string cwd)
        {
            var si = new ProcessStartInfo(exe, args)
            {
                UseShellExecute = false, CreateNoWindow = hidden, WorkingDirectory = cwd,
                RedirectStandardOutput = hidden, RedirectStandardError = hidden
            };
            if (hidden)
            {
                si.StandardOutputEncoding = Encoding.UTF8;
                si.StandardErrorEncoding = Encoding.UTF8;
            }
            si.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
            si.EnvironmentVariables["PYTHONUNBUFFERED"] = "1";
            if (env != null) foreach (var kv in env) si.EnvironmentVariables[kv.Key] = kv.Value;
            return si;
        }

        string CaptureOutput(string exe, string args, Dictionary<string, string> env)
        {
            try
            {
                using (var p = Process.Start(StartInfo(exe, args, env, true, project)))
                {
                    var t = new Thread(() => p.StandardError.ReadToEnd());
                    t.Start();
                    string output = p.StandardOutput.ReadToEnd();
                    p.WaitForExit();
                    t.Join();
                    return output;
                }
            }
            catch { return ""; }
        }

        // Runs a process; with hidden, its output goes to the log (lines ending in \r are progress: status line only)
        int RunProcess(string exe, string args, Dictionary<string, string> env, bool hidden)
        {
            Process p;
            try
            {
                p = Process.Start(StartInfo(exe, args, env, hidden, project));
            }
            catch (Exception ex)
            {
                Log("Could not start " + exe + ": " + ex.Message);
                return -1;
            }
            lock (procLock) current = p;
            Thread errReader = null;
            if (hidden)
            {
                errReader = new Thread(() => Pump(p.StandardError));
                errReader.IsBackground = true;
                errReader.Start();
                Pump(p.StandardOutput);
                errReader.Join();
            }
            p.WaitForExit();
            lock (procLock) current = null;
            int code = p.ExitCode;
            p.Dispose();
            return code;
        }

        void Pump(StreamReader reader)
        {
            var line = new StringBuilder();
            int c;
            bool cr = false;
            while ((c = reader.Read()) >= 0)
            {
                if (cr)
                {
                    cr = false;
                    Emit(line, c != '\n');  // (a \r not followed by \n rewrites the line: progress)
                    if (c == '\n') continue;
                }
                if (c == '\r') cr = true;
                else if (c == '\n') Emit(line, false);
                else line.Append((char)c);
            }
            Emit(line, false);
        }

        void Emit(StringBuilder line, bool progress)
        {
            if (line.Length > 0)
            {
                if (progress) Progress(line.ToString()); else Log(line.ToString());
            }
            line.Clear();
        }

        void Progress(string text)
        {
            var m = Regex.Match(text, @"(\d+)/(\d+)\s*$");
            Ui(() =>
            {
                statusLabel.Text = text.Trim();
                if (m.Success)
                {
                    int done = int.Parse(m.Groups[1].Value), total = Math.Max(1, int.Parse(m.Groups[2].Value));
                    progress.Style = ProgressBarStyle.Blocks;
                    progress.Maximum = total;
                    progress.Value = Math.Min(done, total);
                }
            });
        }

        void KillCurrent()
        {
            Process p;
            lock (procLock) p = current;
            if (p == null) return;
            try
            {
                using (var k = Process.Start(new ProcessStartInfo("taskkill", "/T /F /PID " + p.Id)
                    { UseShellExecute = false, CreateNoWindow = true })) k.WaitForExit(10000);
            }
            catch { }
        }

        void RunWorker(Action action)
        {
            worker = new Thread(() =>
            {
                try { action(); }
                catch (Exception ex) { Log("Launcher error: " + ex); Finish(null, "The step"); }
            });
            worker.IsBackground = true;
            worker.Start();
        }

        void Ui(Action action)
        {
            if (IsDisposed) return;
            try { if (InvokeRequired) BeginInvoke(action); else action(); } catch (ObjectDisposedException) { }
            catch (InvalidOperationException) { }
        }

        void Log(string text)
        {
            text = text.TrimEnd();
            if (text.StartsWith("== ")) Ui(() => statusLabel.Text = text.Substring(3));
            lock (procLock) { try { if (logFile != null) logFile.WriteLine(text); } catch { } }
            Ui(() => log.AppendText(text + Environment.NewLine));
        }

        static string Quote(string s) { return "\"" + s + "\""; }

        void OpenFolder(string folder)
        {
            if (!Directory.Exists(folder)) folder = project;
            Process.Start("explorer.exe", Quote(folder));
        }

        // ---------------------------------------------------------------------------------------------- report
        void CreateReport()
        {
            string run = Path.Combine(project, @"build\game");
            string folder = Path.Combine(project, "reports");
            string zip = Path.Combine(folder, "AstralChain-report-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".zip");
            try
            {
                Directory.CreateDirectory(folder);
                using (var archive = ZipFile.Open(zip, ZipArchiveMode.Create))
                {
                    AddText(archive, "system.txt", SystemInfo());
                    foreach (var f in new[] { @"run.txt", @"round-00\stderr.log", @"round-00\trace.log" })
                    {
                        string p = Path.Combine(run, f);
                        if (File.Exists(p)) AddText(archive, Path.GetFileName(f), Tail(p, 8 << 20));
                    }
                    if (File.Exists(logPath)) AddText(archive, "launcher.log", Tail(logPath, 4 << 20));
                    string dump = Path.Combine(run, "crash.dmp");
                    if (File.Exists(dump) && new FileInfo(dump).Length < (256 << 20))
                        archive.CreateEntryFromFile(dump, "crash.dmp");
                }
                Process.Start("explorer.exe", "/select," + Quote(zip));
                statusLabel.Text = "Report saved: reports\\" + Path.GetFileName(zip);
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Could not create the report: " + ex.Message, "Astral Chain PC");
            }
        }

        // The user's name is replaced in paths (C:\Users\<name>\...)
        static string Redact(string text)
        {
            string user = Environment.UserName;
            if (user.Length > 1)
                text = Regex.Replace(text, @"(?i)(\\Users\\|/Users/)" + Regex.Escape(user) + @"(?=[\\/])", "$1<user>");
            return text;
        }

        static void AddText(ZipArchive archive, string name, string text)
        {
            var entry = archive.CreateEntry(name);
            using (var w = new StreamWriter(entry.Open(), new UTF8Encoding(false))) w.Write(Redact(text));
        }

        static string Tail(string path, int max)
        {
            using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            {
                long start = Math.Max(0, f.Length - max);
                f.Seek(start, SeekOrigin.Begin);
                var text = new StreamReader(f, Encoding.UTF8).ReadToEnd();
                return (start > 0 ? "(first " + start + " bytes left out)\n" : "") + text;
            }
        }

        string SystemInfo()
        {
            var sb = new StringBuilder();
            sb.AppendLine("Report " + DateTime.Now.ToString("s"));
            sb.AppendLine("Project version: " + GitVersion());
            try
            {
                using (var k = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion"))
                    sb.AppendLine("Windows: " + k.GetValue("ProductName") + " " + k.GetValue("DisplayVersion") +
                        " build " + k.GetValue("CurrentBuild") + "." + k.GetValue("UBR"));
            }
            catch { sb.AppendLine("Windows: " + Environment.OSVersion); }
            Wmi(sb, "Win32_Processor", "CPU", "Name", "NumberOfLogicalProcessors");
            Wmi(sb, "Win32_ComputerSystem", "Memory", "TotalPhysicalMemory");
            Wmi(sb, "Win32_VideoController", "GPU", "Name", "DriverVersion", "DriverDate");
            sb.AppendLine();
            sb.AppendLine("Launcher settings:");
            foreach (var o in Options.All) sb.AppendLine("  " + o.Key + "=" + settings.Get(o.Key));
            return sb.ToString();
        }

        static void Wmi(StringBuilder sb, string cls, string label, params string[] props)
        {
            try
            {
                using (var s = new ManagementObjectSearcher("SELECT " + string.Join(",", props) + " FROM " + cls))
                    foreach (ManagementObject o in s.Get())
                        sb.AppendLine(label + ": " + string.Join(", ", props.Select(p => o[p] is ulong
                            ? ((ulong)o[p] / 1073741824.0).ToString("0.0") + " GB" : Convert.ToString(o[p]).Trim())));
            }
            catch { }
        }

        string GitVersion()
        {
            try
            {
                string git = Path.Combine(project, ".git");
                string head = File.ReadAllText(Path.Combine(git, "HEAD")).Trim();
                if (!head.StartsWith("ref: ")) return head;
                string reference = head.Substring(5);
                string file = Path.Combine(git, reference.Replace('/', '\\'));
                if (File.Exists(file)) return reference + " " + File.ReadAllText(file).Trim();
                string packed = Path.Combine(git, "packed-refs");
                if (File.Exists(packed))
                    foreach (var line in File.ReadAllLines(packed))
                        if (line.EndsWith(" " + reference)) return reference + " " + line.Split(' ')[0];
                return reference;
            }
            catch { return "unknown (not a git checkout)"; }
        }
    }

    // ------------------------------------------------------------------------------------------------ look
    static class Theme
    {
        public static readonly Color Accent = Color.FromArgb(64, 214, 255);
        public static readonly Color AccentDeep = Color.FromArgb(24, 110, 255);
        public static readonly Color Text = Color.FromArgb(236, 242, 255);
        public static readonly Color Dim = Color.FromArgb(150, 162, 190);
        public static readonly Color Night = Color.FromArgb(8, 10, 20);

        // The backgrounds embedded in the exe (launcher/art/*.jpg), in file name order
        public static string[] ArtNames()
        {
            return typeof(Theme).Assembly.GetManifestResourceNames().Where(n => n.StartsWith("art/")).OrderBy(n => n)
                .ToArray();
        }

        public static Image LoadArt(string name)
        {
            try
            {
                var s = typeof(Theme).Assembly.GetManifestResourceStream(name);
                return s != null ? Image.FromStream(s) : null;
            }
            catch { return null; }
        }

        // The art scaled to cover the window, darkened on the left and at the bottom for the controls, and the title
        public static Bitmap Backdrop(Image art, Size size, float scale)
        {
            var bmp = new Bitmap(size.Width, size.Height);
            using (var g = Graphics.FromImage(bmp))
            {
                g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
                g.Clear(Night);
                if (art != null)
                {
                    float k = Math.Max(size.Width / (float)art.Width, size.Height / (float)art.Height);
                    float w = art.Width * k, h = art.Height * k;
                    g.DrawImage(art, (size.Width - w) / 2, (size.Height - h) / 2, w, h);
                }
                var r = new Rectangle(0, 0, size.Width, size.Height);
                using (var left = new System.Drawing.Drawing2D.LinearGradientBrush(r, Color.Black, Color.Black, 0f))
                {
                    var blend = new System.Drawing.Drawing2D.ColorBlend
                    {
                        Colors = new[] { Color.FromArgb(235, 6, 8, 18), Color.FromArgb(200, 6, 8, 18),
                            Color.FromArgb(40, 6, 8, 18), Color.FromArgb(0, 6, 8, 18) },
                        Positions = new[] { 0f, 0.3f, 0.55f, 1f }
                    };
                    left.InterpolationColors = blend;
                    g.FillRectangle(left, r);
                }
                using (var bottom = new System.Drawing.Drawing2D.LinearGradientBrush(r, Color.Black, Color.Black, 90f))
                {
                    bottom.InterpolationColors = new System.Drawing.Drawing2D.ColorBlend
                    {
                        Colors = new[] { Color.FromArgb(200, 6, 8, 18), Color.FromArgb(0, 6, 8, 18),
                            Color.FromArgb(0, 6, 8, 18), Color.FromArgb(225, 6, 8, 18) },
                        Positions = new[] { 0f, 0.14f, 0.78f, 1f }
                    };
                    g.FillRectangle(bottom, r);
                }
                g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
                using (var family = new FontFamily("Segoe UI Black"))
                using (var title = new System.Drawing.Drawing2D.GraphicsPath())
                using (var white = new SolidBrush(Text))
                {
                    title.AddString("ASTRAL CHAIN", family, (int)FontStyle.Regular, 54f * scale,
                        new PointF(44 * scale, 44 * scale), StringFormat.GenericTypographic);
                    for (int i = 6; i >= 1; i--)  // (soft glow: wide faint strokes under narrower ones)
                        using (var pen = new Pen(Color.FromArgb(10 + 4 * (6 - i), Accent), i * 2.2f * scale)
                            { LineJoin = System.Drawing.Drawing2D.LineJoin.Round })
                            g.DrawPath(pen, title);
                    g.FillPath(white, title);
                }
                using (var sub = new Font("Segoe UI Semibold", 11f * scale / ScaleOf(g), FontStyle.Regular, GraphicsUnit.Point))
                using (var accent = new SolidBrush(Accent))
                    g.DrawString("N A T I V E   P C   P O R T", sub, accent, 54 * scale, 122 * scale);
                using (var line = new Pen(Color.FromArgb(160, Accent), 2 * scale))
                    g.DrawLine(line, 54 * scale, 117 * scale, 104 * scale, 117 * scale);
                using (var frame = new Pen(Color.FromArgb(90, Accent), 1))
                    g.DrawRectangle(frame, 0, 0, size.Width - 1, size.Height - 1);
            }
            return bmp;
        }

        static float ScaleOf(Graphics g) { return g.DpiY / 96f; }

        [System.Runtime.InteropServices.DllImport("user32.dll")]
        static extern bool ReleaseCapture();
        [System.Runtime.InteropServices.DllImport("user32.dll")]
        static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

        // Moves a borderless window as if its title bar were dragged
        public static void Drag(Form form)
        {
            ReleaseCapture();
            SendMessage(form.Handle, 0xA1, (IntPtr)2, IntPtr.Zero);  // (WM_NCLBUTTONDOWN on the caption)
        }

        // Corners cut at the top left and bottom right, like the game's menus
        public static System.Drawing.Drawing2D.GraphicsPath Cut(RectangleF r, float c)
        {
            var p = new System.Drawing.Drawing2D.GraphicsPath();
            p.AddPolygon(new[] { new PointF(r.Left + c, r.Top), new PointF(r.Right, r.Top), new PointF(r.Right, r.Bottom - c),
                new PointF(r.Right - c, r.Bottom), new PointF(r.Left, r.Bottom), new PointF(r.Left, r.Top + c) });
            return p;
        }
    }

    class NeonButton : Control
    {
        public bool Primary;
        string subtext = "";
        bool hover, down;

        public string Subtext { get { return subtext; } set { subtext = value ?? ""; Invalidate(); } }

        public NeonButton()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint |
                ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            ForeColor = Theme.Text;
            Cursor = Cursors.Hand;
            AccessibleRole = AccessibleRole.PushButton;
        }

        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hover = down = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnMouseDown(MouseEventArgs e) { down = true; Invalidate(); base.OnMouseDown(e); }
        protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }
        protected override void OnEnabledChanged(EventArgs e) { Cursor = Enabled ? Cursors.Hand : Cursors.Default; Invalidate(); base.OnEnabledChanged(e); }
        protected override void OnTextChanged(EventArgs e) { Invalidate(); base.OnTextChanged(e); }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == 0xF5 && Enabled) { OnClick(EventArgs.Empty); return; }  // (BM_CLICK: automation and accessibility tools)
            base.WndProc(ref m);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
            g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
            float s = DeviceScale(g);
            var r = new RectangleF(1, 1, Width - 3, Height - 3);
            bool on = Enabled && (hover || down);
            using (var path = Theme.Cut(r, 12 * s))
            {
                if (!Enabled)
                    using (var b = new SolidBrush(Color.FromArgb(120, 20, 24, 36))) g.FillPath(b, path);
                else if (Primary)
                    using (var b = new System.Drawing.Drawing2D.LinearGradientBrush(r,
                        down ? Theme.AccentDeep : on ? Color.FromArgb(120, 232, 255) : Theme.Accent, Theme.AccentDeep, 0f))
                        g.FillPath(b, path);
                else
                    using (var b = new SolidBrush(on ? Color.FromArgb(200, 18, 46, 78) : Color.FromArgb(165, 10, 14, 28)))
                        g.FillPath(b, path);
                Color edge = !Enabled ? Color.FromArgb(70, Theme.Dim) : on || Primary ? Theme.Accent : Color.FromArgb(120, Theme.Accent);
                using (var pen = new Pen(edge, on ? 2f : 1.2f)) g.DrawPath(pen, path);
            }
            if (!Primary && Enabled)
                using (var bar = new SolidBrush(on ? Color.White : Theme.Accent)) g.FillRectangle(bar, 1, 14 * s, 3 * s, Height - 28 * s);

            Color fore = !Enabled ? Color.FromArgb(120, Theme.Dim) : Primary ? Color.FromArgb(4, 14, 30) : Theme.Text;
            float x = 20 * s;
            bool large = Primary && Height >= 56 * s;
            using (var big = new Font("Segoe UI Semibold", large ? 17f : 11.5f, large ? FontStyle.Bold : FontStyle.Regular))
            using (var small = new Font("Segoe UI", 8.5f))
            using (var fb = new SolidBrush(fore))
            using (var sb = new SolidBrush(!Enabled ? Color.FromArgb(110, Theme.Dim) : Primary ? Color.FromArgb(200, 4, 30, 60) : Theme.Dim))
            {
                var bigSize = g.MeasureString(Text, big);
                if (subtext.Length == 0)
                    g.DrawString(Text, big, fb, x, (Height - bigSize.Height) / 2);
                else
                {
                    var smallSize = g.MeasureString(subtext, small);
                    float top = (Height - bigSize.Height - smallSize.Height + 4 * s) / 2;
                    g.DrawString(Text, big, fb, x, top);
                    g.DrawString(subtext, small, sb, x + 2 * s, top + bigSize.Height - 4 * s);
                }
            }
        }

        static float DeviceScale(Graphics g) { return g.DpiX / 96f; }
    }

    class GlyphButton : Control
    {
        public string Glyph = "";
        public float GlyphSize = 11f;
        bool hover;

        public GlyphButton()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint |
                ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            Cursor = Cursors.Hand;
        }

        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == 0xF5 && Enabled) { OnClick(EventArgs.Empty); return; }  // (BM_CLICK: automation and accessibility tools)
            base.WndProc(ref m);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
            using (var b = new SolidBrush(hover ? Color.FromArgb(150, Theme.AccentDeep) : Color.FromArgb(130, 6, 8, 18)))
                g.FillRectangle(b, ClientRectangle);
            using (var f = new Font("Segoe UI", GlyphSize))
            using (var b = new SolidBrush(hover ? Color.White : Theme.Text))
            {
                var sz = g.MeasureString(Glyph, f);
                g.DrawString(Glyph, f, b, (Width - sz.Width) / 2, (Height - sz.Height) / 2);
            }
        }
    }

    class NeonProgress : Control
    {
        ProgressBarStyle style = ProgressBarStyle.Blocks;
        int max = 100, value;
        float phase;
        readonly System.Windows.Forms.Timer timer = new System.Windows.Forms.Timer { Interval = 16 };

        public NeonProgress()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint |
                ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            timer.Tick += (s, e) => { phase = (phase + 0.008f) % 1.4f; Invalidate(); };
        }

        public ProgressBarStyle Style
        {
            get { return style; }
            set { style = value; timer.Enabled = value == ProgressBarStyle.Marquee; Invalidate(); }
        }
        public int Maximum { get { return max; } set { max = Math.Max(1, value); Invalidate(); } }
        public int Value { get { return value; } set { this.value = Math.Max(0, Math.Min(max, value)); Invalidate(); } }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            using (var track = new SolidBrush(Color.FromArgb(110, 30, 40, 64))) g.FillRectangle(track, ClientRectangle);
            RectangleF fill;
            if (style == ProgressBarStyle.Marquee) fill = new RectangleF((phase - 0.4f) * Width, 0, 0.4f * Width, Height);
            else fill = new RectangleF(0, 0, Width * (float)value / max, Height);
            if (fill.Width <= 0) return;
            using (var b = new System.Drawing.Drawing2D.LinearGradientBrush(new RectangleF(fill.X - 1, 0, fill.Width + 2, Height),
                Theme.AccentDeep, Theme.Accent, 0f))
                g.FillRectangle(b, fill);
        }

        protected override void Dispose(bool disposing) { if (disposing) timer.Dispose(); base.Dispose(disposing); }
    }

    // ------------------------------------------------------------------------------------------------ settings window
    class SettingsForm : Form
    {
        readonly Settings settings;
        readonly Dictionary<string, Control> inputs = new Dictionary<string, Control>();
        static readonly Color Field = Color.FromArgb(24, 30, 46);

        public SettingsForm(Settings settings)
        {
            this.settings = settings;
            Text = "Astral Chain PC settings";
            Font = new Font("Segoe UI", 9F);
            FormBorderStyle = FormBorderStyle.None;
            ShowInTaskbar = false;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(560, 640);
            BackColor = Color.FromArgb(14, 17, 28);
            ForeColor = Theme.Text;

            // Header: title, close button; the window is dragged by it
            var header = new Label { Text = "SETTINGS", Font = new Font("Segoe UI Semibold", 12F), ForeColor = Theme.Text,
                Location = new Point(1, 1), Size = new Size(558, 42), Padding = new Padding(14, 0, 0, 0),
                TextAlign = ContentAlignment.MiddleLeft, BackColor = Color.FromArgb(9, 11, 20) };
            header.MouseDown += (s, e) =>
            {
                if (e.Button != MouseButtons.Left) return;
                Theme.Drag(this);
            };
            var close = new GlyphButton { Glyph = "\u2715", AccessibleName = "Close", Location = new Point(518, 7), Size = new Size(34, 30) };
            close.Click += (s, e) => DialogResult = DialogResult.Cancel;
            Controls.Add(close);
            Controls.Add(header);
            Paint += (s, e) =>
            {
                using (var pen = new Pen(Color.FromArgb(150, Theme.Accent))) e.Graphics.DrawRectangle(pen, 0, 0, ClientSize.Width - 1, ClientSize.Height - 1);
                using (var line = new Pen(Theme.Accent, 2)) e.Graphics.DrawLine(line, 1, 43, 120, 43);
            };

            var panel = new Panel { Location = new Point(1, 44), Size = new Size(558, 544), AutoScroll = true };
            Controls.Add(panel);
            int y = 10;
            string group = null;
            foreach (var o in Options.All)
            {
                if (o.Group != group)
                {
                    group = o.Group;
                    panel.Controls.Add(new Label { Text = group.ToUpperInvariant(), Font = new Font("Segoe UI", 10F, FontStyle.Bold),
                        Location = new Point(14, y), AutoSize = true, ForeColor = Theme.Accent });
                    y += 28;
                }
                string value = settings.Get(o.Key);
                Control input;
                if (o.Kind == OptKind.Check)
                {
                    input = new CheckBox { Text = o.Label, Checked = value == "1", Location = new Point(20, y),
                        Size = new Size(500, 22) };
                    panel.Controls.Add(input);
                    y += 22;
                }
                else
                {
                    panel.Controls.Add(new Label { Text = o.Label, Location = new Point(20, y + 3), AutoSize = true });
                    if (o.Kind == OptKind.Choice)
                    {
                        var combo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, FlatStyle = FlatStyle.Flat,
                            BackColor = Field, ForeColor = Theme.Text, Location = new Point(250, y), Size = new Size(270, 24) };
                        foreach (var c in o.Choices) combo.Items.Add(c.Split('|')[1]);
                        int index = Array.FindIndex(o.Choices, c => c.Split('|')[0] == value);
                        combo.SelectedIndex = index < 0 ? 0 : index;
                        input = combo;
                    }
                    else if (o.Kind == OptKind.Number)
                    {
                        int n;
                        if (!int.TryParse(value, out n)) n = int.Parse(o.Default);
                        input = new NumericUpDown { Minimum = o.Min, Maximum = o.Max, Increment = 100,
                            Value = Math.Max(o.Min, Math.Min(o.Max, n)), Location = new Point(250, y),
                            Size = new Size(100, 24), BackColor = Field, ForeColor = Theme.Text, BorderStyle = BorderStyle.FixedSingle };
                    }
                    else
                        input = new TextBox { Text = value, Location = new Point(250, y), Size = new Size(270, 24),
                            BackColor = Field, ForeColor = Theme.Text, BorderStyle = BorderStyle.FixedSingle };
                    panel.Controls.Add(input);
                    y += 28;
                }
                inputs[o.Key] = input;
                var help = new Label { Text = o.Help, ForeColor = Theme.Dim, Location = new Point(38, y), Font = Font,
                    Size = new Size(482, 32) };
                help.Height = TextRenderer.MeasureText(o.Help, help.Font, new Size(482, 1000),
                    TextFormatFlags.WordBreak).Height + 2;
                panel.Controls.Add(help);
                y += help.Height + 8;
            }
            panel.Controls.Add(new Label { Location = new Point(0, y), Size = new Size(1, 1) });

            var defaults = new NeonButton { Text = "Defaults", Location = new Point(14, 596), Size = new Size(100, 34) };
            defaults.Click += (s, e) => LoadDefaults();
            var ok = new NeonButton { Text = "Save", Primary = true, Location = new Point(338, 596), Size = new Size(100, 34) };
            ok.Click += (s, e) => { Store(); DialogResult = DialogResult.OK; };
            var cancelButton = new NeonButton { Text = "Cancel", Location = new Point(446, 596), Size = new Size(100, 34) };
            cancelButton.Click += (s, e) => DialogResult = DialogResult.Cancel;
            Controls.Add(defaults);
            Controls.Add(ok);
            Controls.Add(cancelButton);
            KeyPreview = true;
            KeyDown += (s, e) => { if (e.KeyCode == Keys.Escape) DialogResult = DialogResult.Cancel; };
            AutoScaleDimensions = new SizeF(96F, 96F);
            AutoScaleMode = AutoScaleMode.Dpi;
        }

        void LoadDefaults()
        {
            foreach (var o in Options.All) ShowValue(o, o.Default);
        }

        void ShowValue(Opt o, string value)
        {
            var c = inputs[o.Key];
            if (c is CheckBox) ((CheckBox)c).Checked = value == "1";
            else if (c is ComboBox)
            {
                int index = Array.FindIndex(o.Choices, x => x.Split('|')[0] == value);
                ((ComboBox)c).SelectedIndex = index < 0 ? 0 : index;
            }
            else if (c is NumericUpDown) ((NumericUpDown)c).Value = int.Parse(value);
            else c.Text = value;
        }

        void Store()
        {
            foreach (var o in Options.All)
            {
                var c = inputs[o.Key];
                string v;
                if (c is CheckBox) v = ((CheckBox)c).Checked ? "1" : "0";
                else if (c is ComboBox) v = o.Choices[Math.Max(0, ((ComboBox)c).SelectedIndex)].Split('|')[0];
                else if (c is NumericUpDown) v = ((int)((NumericUpDown)c).Value).ToString();
                else v = c.Text.Trim();
                settings.Set(o.Key, v);
            }
        }
    }

    // ------------------------------------------------------------------------------------------------ controls window
    // Keyboard keys for the Switch controller's buttons and sticks. Kept in launcher.ini as keys=A:74,B:75,... (Windows
    // virtual-key codes) and passed to the game as AC_KEYS (runtime/hid_service.h); unchanged keys pass nothing.
    class ControlsForm : Form
    {
        // name (as AC_KEYS), label, default key
        static readonly string[][] Actions = {
            new[] { "A", "A", "74" }, new[] { "B", "B", "75" }, new[] { "X", "X", "73" }, new[] { "Y", "Y", "85" },
            new[] { "L", "L", "81" }, new[] { "R", "R", "69" }, new[] { "ZL", "ZL", "90" }, new[] { "ZR", "ZR", "67" },
            new[] { "PLUS", "+ (Plus)", "13" }, new[] { "MINUS", "- (Minus)", "8" },
            new[] { "LSTICK", "Left stick press", "49" }, new[] { "RSTICK", "Right stick press", "50" },
            new[] { "LUP", "Left stick up", "87" }, new[] { "LDOWN", "Left stick down", "83" },
            new[] { "LLEFT", "Left stick left", "65" }, new[] { "LRIGHT", "Left stick right", "68" },
            new[] { "RUP", "Right stick up", "84" }, new[] { "RDOWN", "Right stick down", "71" },
            new[] { "RLEFT", "Right stick left", "70" }, new[] { "RRIGHT", "Right stick right", "72" },
            new[] { "DUP", "D-pad up", "38" }, new[] { "DDOWN", "D-pad down", "40" },
            new[] { "DLEFT", "D-pad left", "37" }, new[] { "DRIGHT", "D-pad right", "39" },
        };

        readonly Settings settings;
        readonly Dictionary<string, int> keys;
        readonly Dictionary<string, NeonButton> buttons = new Dictionary<string, NeonButton>();
        readonly Label hint;
        string capturing;

        [DllImport("user32.dll")] static extern uint MapVirtualKey(uint code, uint mapType);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetKeyNameText(int lParam, StringBuilder text, int size);

        static Dictionary<string, int> Defaults()
        {
            return Actions.ToDictionary(a => a[0], a => int.Parse(a[2]));
        }

        static Dictionary<string, int> LoadKeys(Settings settings)
        {
            var keys = Defaults();
            foreach (var part in (settings.Raw("keys") ?? "").Split(','))
            {
                var kv = part.Split(':');
                int vk;
                if (kv.Length == 2 && keys.ContainsKey(kv[0]) && int.TryParse(kv[1], out vk) && vk >= 0 && vk < 256) keys[kv[0]] = vk;
            }
            return keys;
        }

        static string Join(Dictionary<string, int> keys)
        {
            return string.Join(",", Actions.Select(a => a[0] + ":" + keys[a[0]]));
        }

        // "AC_KEYS=..." for the game, or "" when every key is the default
        public static string Env(Settings settings)
        {
            var keys = LoadKeys(settings);
            return Join(keys) == Join(Defaults()) ? "" : "AC_KEYS=" + Join(keys);
        }

        // The key's name on this keyboard layout ("J", "Enter", "Left Shift" ...)
        public static string KeyName(int vk)
        {
            if (vk == 0) return "(none)";
            uint scan = MapVirtualKey((uint)vk, 0);
            bool extended = (vk >= 33 && vk <= 40) || vk == 45 || vk == 46 || vk == 111 || vk == 144;
            var text = new StringBuilder(64);
            if (scan != 0) GetKeyNameText((int)(scan << 16) | (extended ? 1 << 24 : 0), text, text.Capacity);
            string name = text.ToString();
            if (name.Length > 1 && name == name.ToUpperInvariant())  // ("ENTER" -> "Enter")
                name = System.Globalization.CultureInfo.CurrentCulture.TextInfo.ToTitleCase(name.ToLowerInvariant());
            return name.Length > 0 ? name : "Key " + vk;
        }

        public ControlsForm(Settings settings)
        {
            this.settings = settings;
            keys = LoadKeys(settings);
            Text = "Astral Chain PC controls";
            Font = new Font("Segoe UI", 9F);
            FormBorderStyle = FormBorderStyle.None;
            ShowInTaskbar = false;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(640, 560);
            BackColor = Color.FromArgb(14, 17, 28);
            ForeColor = Theme.Text;

            var header = new Label { Text = "CONTROLS", Font = new Font("Segoe UI Semibold", 12F), ForeColor = Theme.Text,
                Location = new Point(1, 1), Size = new Size(638, 42), Padding = new Padding(14, 0, 0, 0),
                TextAlign = ContentAlignment.MiddleLeft, BackColor = Color.FromArgb(9, 11, 20) };
            header.MouseDown += (s, e) => { if (e.Button == MouseButtons.Left) Theme.Drag(this); };
            var close = new GlyphButton { Glyph = "\u2715", AccessibleName = "Close", Location = new Point(598, 7), Size = new Size(34, 30) };
            close.Click += (s, e) => DialogResult = DialogResult.Cancel;
            Controls.Add(close);
            Controls.Add(header);
            Paint += (s, e) =>
            {
                using (var pen = new Pen(Color.FromArgb(150, Theme.Accent))) e.Graphics.DrawRectangle(pen, 0, 0, ClientSize.Width - 1, ClientSize.Height - 1);
                using (var line = new Pen(Theme.Accent, 2)) e.Graphics.DrawLine(line, 1, 43, 120, 43);
            };

            string[] titles = { "BUTTONS", "STICKS AND D-PAD" };
            for (int col = 0; col < 2; col++)
            {
                int x = 20 + col * 310, y = 58;
                Controls.Add(new Label { Text = titles[col], Font = new Font("Segoe UI", 10F, FontStyle.Bold), Location = new Point(x, y),
                    AutoSize = true, ForeColor = Theme.Accent });
                y += 30;
                for (int i = col * 12; i < col * 12 + 12; i++)
                {
                    var a = Actions[i];
                    Controls.Add(new Label { Text = a[1], Location = new Point(x, y + 6), AutoSize = true });
                    var b = new NeonButton { Location = new Point(x + 150, y), Size = new Size(140, 30), AccessibleName = "Key for " + a[1] };
                    string name = a[0];
                    b.Click += (s, e) => StartCapture(name);
                    buttons[name] = b;
                    Controls.Add(b);
                    y += 33;
                }
            }
            hint = new Label { Location = new Point(20, 488), Size = new Size(600, 20), ForeColor = Theme.Dim,
                Text = "Click a key, then press the new key (Esc: keep the old one). A key already in use swaps places." };
            Controls.Add(hint);
            ShowKeys();

            var defaults = new NeonButton { Text = "Defaults", Location = new Point(14, 516), Size = new Size(100, 34) };
            defaults.Click += (s, e) => { capturing = null; foreach (var kv in Defaults()) keys[kv.Key] = kv.Value; ShowKeys(); };
            var ok = new NeonButton { Text = "Save", Primary = true, Location = new Point(418, 516), Size = new Size(100, 34) };
            ok.Click += (s, e) => { settings.Set("keys", Join(keys)); DialogResult = DialogResult.OK; };
            var cancelButton = new NeonButton { Text = "Cancel", Location = new Point(526, 516), Size = new Size(100, 34) };
            cancelButton.Click += (s, e) => DialogResult = DialogResult.Cancel;
            Controls.Add(defaults);
            Controls.Add(ok);
            Controls.Add(cancelButton);
            AutoScaleDimensions = new SizeF(96F, 96F);
            AutoScaleMode = AutoScaleMode.Dpi;
        }

        void ShowKeys()
        {
            foreach (var a in Actions)
            {
                var b = buttons[a[0]];
                b.Text = capturing == a[0] ? "Press a key..." : KeyName(keys[a[0]]);
                b.Primary = capturing == a[0];
                b.Invalidate();
            }
        }

        void StartCapture(string name)
        {
            capturing = name;
            buttons[name].Focus();
            ShowKeys();
        }

        // Every key press reaches here first (arrows, Tab, Enter and Alt included): the raw virtual-key code, as the
        // game window receives it.
        protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
        {
            const int WM_KEYDOWN = 0x100, WM_SYSKEYDOWN = 0x104;
            if (msg.Msg != WM_KEYDOWN && msg.Msg != WM_SYSKEYDOWN) return base.ProcessCmdKey(ref msg, keyData);
            int vk = (int)msg.WParam & 0xFF;
            if (capturing == null)
            {
                if (vk == 27) { DialogResult = DialogResult.Cancel; return true; }
                return base.ProcessCmdKey(ref msg, keyData);
            }
            if (vk == 27) { capturing = null; ShowKeys(); return true; }
            if (vk == 91 || vk == 92 || vk < 8) return true;  // (Windows keys; mouse buttons)
            string other = keys.Where(kv => kv.Value == vk && kv.Key != capturing).Select(kv => kv.Key).FirstOrDefault();
            if (other != null) keys[other] = keys[capturing];  // (swapped)
            keys[capturing] = vk;
            capturing = null;
            ShowKeys();
            return true;
        }
    }
}
