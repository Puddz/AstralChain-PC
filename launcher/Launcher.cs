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
            Add("ignore_focus", "Game", "Keep running while the game window is in the background",
                "Off: the game pauses when its window loses focus, as on the Switch's home menu.", OptKind.Check, "0",
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
            Add("skip_unbuilt", "Shaders and performance", "Do not wait for shaders that are still compiling",
                "Fewer pauses the first time something is drawn, but it can appear a moment late.", OptKind.Check, "0",
                OptTarget.Game, When("1", "LARECOMP_SKIP_UNBUILT=1"));
            Add("parallel", "Shaders and performance", "Run the game's threads in parallel",
                "Needed for 60 fps. Turn off only to test whether a problem depends on it.", OptKind.Check, "1",
                OptTarget.Game, When("1", "AC_PARALLEL=1"));
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
            hang.Min = 500; hang.Max = 1800;
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
                string e = o.Env(Get(o.Key));
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
        readonly Button install, shaders, play, playDiag, settingsButton, cancel, report;
        readonly Label stateLabel, statusLabel;
        readonly ProgressBar progress;
        readonly TextBox log;
        readonly LinkLabel savesLink, logsLink;
        StreamWriter logFile;
        Thread worker;
        readonly object procLock = new object();
        Process current;
        bool cancelled, gameRunning;
        string shaderState = "", buildState = "";
        bool installed;

        public MainForm(string project)
        {
            this.project = project;
            venvPython = Path.Combine(project, @".venv\Scripts\python.exe");
            settings = new Settings(Path.Combine(project, @"userdata\launcher.ini"));
            logPath = Path.Combine(project, @"userdata\launcher.log");

            Text = "Astral Chain PC";
            Font = new Font("Segoe UI", 9F);
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(640, 600);
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            var title = new Label { Text = "Astral Chain PC", Font = new Font("Segoe UI", 18F, FontStyle.Bold),
                AutoSize = true, Location = new Point(18, 12) };
            stateLabel = new Label { Text = "Checking...", AutoSize = false, Location = new Point(21, 52),
                Size = new Size(600, 20), ForeColor = Color.DimGray };
            Controls.Add(title);
            Controls.Add(stateLabel);

            int y = 84;
            install = MakeButton("Install game", y, OnInstall); y += 50;
            shaders = MakeButton("Compile shaders", y, OnShaders); y += 50;
            play = MakeButton("Play", y, (s, e) => OnPlay(false)); y += 50;
            playDiag = MakeButton("Play with diagnostics", y, (s, e) => OnPlay(true)); y += 50;
            settingsButton = MakeButton("Settings", y, OnSettings);
            play.Font = new Font("Segoe UI", 11F, FontStyle.Bold);

            var help = new Label { AutoSize = false, Location = new Point(330, 84), Size = new Size(290, 240),
                ForeColor = Color.DimGray, Text =
                "Install: picks your prod.keys, the base game (XCI or NSP) and the v1.0.1 update, then builds the " +
                "game in this folder (15-40 minutes, about 35 GB free needed).\n\n" +
                "Compile shaders: prepares every shader for your GPU, so the game does not pause to compile them. " +
                "Play does this too when needed.\n\n" +
                "Play with diagnostics: logs where the time of slow frames goes and saves a crash dump if the game " +
                "crashes. Afterwards, Create report packs the logs to attach to a bug report." };
            Controls.Add(help);

            report = new Button { Text = "Create report", Location = new Point(330, 334), Size = new Size(140, 30) };
            report.Click += (s, e) => CreateReport();
            Controls.Add(report);
            savesLink = new LinkLabel { Text = "Saves folder", AutoSize = true, Location = new Point(484, 341) };
            savesLink.Click += (s, e) => OpenFolder(Path.Combine(project, @"userdata\saves"));
            logsLink = new LinkLabel { Text = "Logs folder", AutoSize = true, Location = new Point(566, 341) };
            logsLink.Click += (s, e) => OpenFolder(Path.Combine(project, @"build\game\round-00"));
            Controls.Add(savesLink);
            Controls.Add(logsLink);

            statusLabel = new Label { AutoSize = false, Location = new Point(21, 378), Size = new Size(500, 20) };
            cancel = new Button { Text = "Cancel", Location = new Point(530, 372), Size = new Size(90, 28),
                Enabled = false };
            cancel.Click += OnCancel;
            progress = new ProgressBar { Location = new Point(21, 404), Size = new Size(599, 14) };
            log = new TextBox { Location = new Point(21, 428), Size = new Size(599, 156), Multiline = true,
                ReadOnly = true, ScrollBars = ScrollBars.Vertical, Font = new Font("Consolas", 8.5F),
                BackColor = SystemColors.Window, WordWrap = true };
            Controls.Add(statusLabel);
            Controls.Add(cancel);
            Controls.Add(progress);
            Controls.Add(log);

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

        Button MakeButton(string text, int y, EventHandler click)
        {
            var b = new Button { Text = text, Location = new Point(20, y), Size = new Size(290, 40) };
            b.Click += click;
            Controls.Add(b);
            return b;
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
            install.Text = installed ? "Install game  (installed)" : "Install game";
            stateLabel.Text = installed
                ? "Installed in " + project + (buildState == "stale" ? "  (updated: rebuilds before the next start)" : "")
                : "Not installed yet. Start with Install game.";
            string note = !installed ? "" : shaderState == "compiled" ? "  (Already compiled)"
                : shaderState == "needed" || buildState != "current" ? "  (Needs recompiling)" : "  (Not compiled yet)";
            shaders.Text = "Compile shaders" + note;
            UpdateButtons(false);
        }

        void UpdateButtons(bool busy)
        {
            install.Enabled = !busy && !installed;
            shaders.Enabled = !busy && installed;
            play.Enabled = !busy && installed;
            playDiag.Enabled = !busy && installed;
            settingsButton.Enabled = !busy;
            report.Enabled = !busy && File.Exists(Path.Combine(project, @"build\game\run.txt"));
        }

        void SetBusy(bool busy, string status, bool cancellable)
        {
            UpdateButtons(busy);
            cancel.Enabled = busy && cancellable;
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
                    progress.Style = ProgressBarStyle.Marquee; cancel.Enabled = false; });
                string run = Path.Combine(project, @"build\game");
                string dump = Path.Combine(run, "crash.dmp");
                if (diagnostics) try { File.Delete(dump); } catch { }
                string env = "AC_PARALLEL=;AC_IGNORE_FOCUS=;" + settings.Env(OptTarget.Game) +
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
            using (var f = new SettingsForm(settings))
            {
                if (f.ShowDialog(this) == DialogResult.OK)
                {
                    settings.Save();
                    RefreshState();
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
        bool EnsurePython()
        {
            if (File.Exists(venvPython)) return true;
            Log("== Creating the Python environment");
            foreach (var v in new[] { "3.14", "3.13", "3.12" })
            {
                if (CaptureOutput("py", "-" + v + " -c \"print('ok')\"", null).Trim() != "ok") continue;
                return RunProcess("py", "-" + v + " -m venv .venv", null, true) == 0 && File.Exists(venvPython);
            }
            if (CaptureOutput("python", "-c \"import sys; print('ok' if sys.version_info >= (3, 12) else 'old')\"", null)
                .Trim() == "ok")
                return RunProcess("python", "-m venv .venv", null, true) == 0 && File.Exists(venvPython);
            Ui(() =>
            {
                if (MessageBox.Show(this, "Python 3.12 or newer is needed to build the game.\n\nInstall it from " +
                    "python.org (keep the \"py launcher\" option ticked), then press Install game again.\n\n" +
                    "Open the download page now?", "Python needed", MessageBoxButtons.YesNo,
                    MessageBoxIcon.Information) == DialogResult.Yes)
                    Process.Start("https://www.python.org/downloads/windows/");
            });
            Log("Python 3.12 or newer was not found.");
            return false;
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

    // ------------------------------------------------------------------------------------------------ settings window
    class SettingsForm : Form
    {
        readonly Settings settings;
        readonly Dictionary<string, Control> inputs = new Dictionary<string, Control>();

        public SettingsForm(Settings settings)
        {
            this.settings = settings;
            Text = "Astral Chain PC settings";
            Font = new Font("Segoe UI", 9F);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            ShowInTaskbar = false;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(560, 600);

            var panel = new Panel { Location = new Point(0, 0), Size = new Size(560, 548), AutoScroll = true };
            Controls.Add(panel);
            int y = 10;
            string group = null;
            foreach (var o in Options.All)
            {
                if (o.Group != group)
                {
                    group = o.Group;
                    panel.Controls.Add(new Label { Text = group, Font = new Font("Segoe UI", 10F, FontStyle.Bold),
                        Location = new Point(14, y), AutoSize = true });
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
                        var combo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList,
                            Location = new Point(250, y), Size = new Size(270, 24) };
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
                            Size = new Size(100, 24) };
                    }
                    else
                        input = new TextBox { Text = value, Location = new Point(250, y), Size = new Size(270, 24) };
                    panel.Controls.Add(input);
                    y += 28;
                }
                inputs[o.Key] = input;
                var help = new Label { Text = o.Help, ForeColor = Color.DimGray, Location = new Point(38, y), Font = Font,
                    Size = new Size(482, 32) };
                help.Height = TextRenderer.MeasureText(o.Help, help.Font, new Size(482, 1000),
                    TextFormatFlags.WordBreak).Height + 2;
                panel.Controls.Add(help);
                y += help.Height + 8;
            }
            panel.Controls.Add(new Label { Location = new Point(0, y), Size = new Size(1, 1) });

            var defaults = new Button { Text = "Defaults", Location = new Point(14, 558), Size = new Size(90, 30) };
            defaults.Click += (s, e) => LoadDefaults();
            var ok = new Button { Text = "Save", Location = new Point(360, 558), Size = new Size(90, 30),
                DialogResult = DialogResult.OK };
            ok.Click += (s, e) => Store();
            var cancelButton = new Button { Text = "Cancel", Location = new Point(456, 558), Size = new Size(90, 30),
                DialogResult = DialogResult.Cancel };
            Controls.Add(defaults);
            Controls.Add(ok);
            Controls.Add(cancelButton);
            AcceptButton = ok;
            CancelButton = cancelButton;
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
}
