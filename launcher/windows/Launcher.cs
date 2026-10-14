using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

[assembly: AssemblyTitle("RGO Launcher")]
[assembly: AssemblyDescription("Local ROM build and launcher for RGO")]
[assembly: AssemblyVersion("0.1.0.0")]
[assembly: AssemblyInformationalVersion("0.1.0")]

namespace JfgLauncher
{
    // One embedded brand asset is shared by setup, support and legacy tool windows.
    internal class ApplicationWindow : Form
    {
        private static readonly Icon AppIcon = LoadApplicationIcon();
        private static Icon LoadApplicationIcon()
        {
            using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("JfgLauncher.AppIcon"))
            using (var icon = new Icon(stream))
                return (Icon)icon.Clone();
        }
        protected ApplicationWindow() { Icon = AppIcon; }
    }

    [DataContract]
    internal sealed class Settings
    {
        [DataMember] public string RuntimePath = "";
        [DataMember] public string RomPath = "";
    }

    internal static class LocalSetup
    {
        internal const long RomSize = 33554432;
        internal const string RomSha1 = "493ced9008dbe932d6e91179b68e8630cf23a023";
        internal static readonly string Guide = "https://github.com/ravenx444/RGO-Recomp/blob/" + BuildInfo.SourceCommit + "/docs/rgo/PLAYER_GUIDE.md";

        internal static string ProfileRoot
        {
            get
            {
                if (!String.IsNullOrEmpty(Environment.GetEnvironmentVariable("RGO_DATA_ROOT")))
                    return Path.Combine(FirstRun.Root, "profile");
                return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "JFGRecomp", "profiles", "default");
            }
        }

        internal static string PreferredRuntime(string directory, string savedRuntime)
        {
            string paired = Path.Combine(directory, "jfg-native-boot.exe");
            if (File.Exists(paired))
            {
                try { return ValidateRuntime(paired); }
                catch (InvalidDataException) { }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
            return savedRuntime ?? "";
        }

        internal static string NavigationProfile(string profile)
        {
            return Path.Combine(Path.GetDirectoryName(FullPath(profile)), "navigation-mod");
        }

        internal static string PrepareNavigationProfile(string profile)
        {
            string target = NavigationProfile(profile);
            Directory.CreateDirectory(target);
            string marker = Path.Combine(target, "initialized.txt");
            if (!File.Exists(marker))
            {
                foreach (string name in new string[] {"jfg.flash", "controller-1.pak"})
                {
                    string source = Path.Combine(profile, name), destination = Path.Combine(target, name);
                    if (File.Exists(source) && !File.Exists(destination)) File.Copy(source, destination, false);
                }
                File.WriteAllText(marker, "Separate navigation mod save profile.\n");
            }
            // Controller settings remain shared; campaign saves never are.
            string controller = ControllerProfile.FileName(profile);
            if (File.Exists(controller))
            {
                ControllerProfile.Load(profile);
                File.Copy(controller, ControllerProfile.FileName(target), true);
            }
            return target;
        }

        internal static ProcessStartInfo NavigationStartInfo(string runtime, string rom, string profile)
        {
            string target = PrepareNavigationProfile(profile);
            ProcessStartInfo info = StartInfo(runtime, rom, target);
            info.EnvironmentVariables["JFG_MASTER_VOLUME_CONFIG"] = AudioPreferences.FileName(profile);
            info.EnvironmentVariables["JFG_VIDEO_CONFIG"] = VideoPreferences.FileName(profile);
            // Each run has a new export directory, so old maps cannot look current.
            string exports = Path.Combine(target, "maps", DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N"));
            info.EnvironmentVariables["JFG_NAVIGATION_MOD"] = "1";
            info.EnvironmentVariables["JFG_MOD_OUTPUT"] = exports;
            MapModPreferences.PrepareSession(profile,exports);
            return info;
        }

        internal static ProcessStartInfo LiveToolsStartInfo(string runtime,string rom,string profile) {
            var info=StartInfo(runtime,rom,profile);
            info.EnvironmentVariables["JFG_LIVE_OUTPUT"]=Path.Combine(profile,"maps",DateTime.UtcNow.ToString("yyyyMMdd-HHmmss")+"-"+Guid.NewGuid().ToString("N"));
            MapModPreferences.PrepareSession(profile,info.EnvironmentVariables["JFG_LIVE_OUTPUT"]);
            return info;
        }

        internal static string FindSourceRoot(string start)
        {
            DirectoryInfo directory = new DirectoryInfo(Path.GetFullPath(start));
            for (int depth = 0; directory != null && depth < 8; depth++, directory = directory.Parent)
            {
                if (File.Exists(Path.Combine(directory.FullName, "scripts", "build_from_rom.py")) &&
                    File.Exists(Path.Combine(directory.FullName, "CMakeLists.txt"))) return directory.FullName;
            }
            return null;
        }

        // RGO: a frontend started by scripts/build_from_rom.py (JFG_SOURCE_ROOT)
        // rebuilds the game from that checkout instead of the upstream download.
        internal static ProcessStartInfo SourceSetupStartInfo(string sourceRoot, string romPath)
        {
            ProcessStartInfo info = BuildStartInfo(sourceRoot, romPath);
            info.Arguments = "-u " + info.Arguments + " --no-frontend";
            return info;
        }

        internal static ProcessStartInfo BuildStartInfo(string sourceRoot, string romPath)
        {
            string root = FullPath(sourceRoot);
            if (FindSourceRoot(root) != root)
                throw new InvalidDataException("Select the source repository folder containing scripts/build_from_rom.py.");
            ProcessStartInfo info = new ProcessStartInfo();
            info.FileName = "python";
            info.Arguments = Quote(Path.Combine(root, "scripts", "build_from_rom.py")) + " --rom " + Quote(FullPath(romPath));
            info.WorkingDirectory = root;
            info.UseShellExecute = false;
            info.CreateNoWindow = true;
            info.RedirectStandardOutput = true;
            info.RedirectStandardError = true;
            return info;
        }

        internal static string FullPath(string path)
        {
            if (String.IsNullOrWhiteSpace(path) || path.IndexOfAny(new char[] {'\r', '\n', '\0', '"'}) >= 0)
                throw new InvalidDataException("Select a valid local file.");
            string full = Path.GetFullPath(path);
            if (full.StartsWith(@"\\", StringComparison.Ordinal))
                throw new InvalidDataException("Choose a file on a local drive, outside network shares.");
            return full;
        }

        internal static string Digest(Stream input)
        {
            using (SHA1 sha = SHA1.Create())
                return BitConverter.ToString(sha.ComputeHash(input)).Replace("-", "").ToLowerInvariant();
        }

        internal static void ValidateRom(Stream input, long size, string expectedHash)
        {
            if (!input.CanSeek || input.Length != size)
                throw new InvalidDataException("Unsupported ROM size. Select the 32 MiB North American retail ROM.");
            input.Position = 0;
            byte[] header = new byte[4];
            if (input.Read(header, 0, 4) != 4 || header[0] != 0x80 || header[1] != 0x37 ||
                header[2] != 0x12 || header[3] != 0x40)
                throw new InvalidDataException("Unsupported byte order. This preview requires a big-endian .z64 ROM.");
            input.Position = 0;
            if (!String.Equals(Digest(input), expectedHash, StringComparison.Ordinal))
                throw new InvalidDataException("This ROM does not match the supported North American retail version.");
            input.Position = 0;
        }

        internal static FileStream OpenVerifiedRom(string path)
        {
            // Keep the read lock until the game exits so the checked ROM cannot be replaced or modified.
            FileStream input = new FileStream(FullPath(path), FileMode.Open, FileAccess.Read, FileShare.Read);
            try { ValidateRom(input, RomSize, RomSha1); return input; }
            catch { input.Dispose(); throw; }
        }

        internal static void ValidatePe(string path)
        {
            using (FileStream input = File.OpenRead(path))
            using (BinaryReader reader = new BinaryReader(input))
            {
                if (input.Length < 128 || reader.ReadUInt16() != 0x5a4d)
                    throw new InvalidDataException("The selected build contains an invalid Windows executable or library.");
                input.Position = 60;
                uint offset = reader.ReadUInt32();
                if (offset > input.Length - 26)
                    throw new InvalidDataException("The selected build contains an invalid Windows executable or library.");
                input.Position = offset;
                if (reader.ReadUInt32() != 0x00004550 || reader.ReadUInt16() != 0x8664)
                    throw new InvalidDataException("Select a Windows x64 native game build.");
            }
        }

        internal static string ValidateRuntime(string path)
        {
            string full = FullPath(path);
            if (!String.Equals(Path.GetFileName(full), "jfg-native-boot.exe", StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Select jfg-native-boot.exe from a live game build. The jfg.exe host shell cannot play the game.");
            if (!File.Exists(full))
                throw new InvalidDataException("The native game build is missing. Use Build from ROM or select an existing build.");
            ValidatePe(full);
            foreach (string name in new string[] {"SDL2.dll", "dxcompiler.dll", "dxil.dll"})
            {
                string library = Path.Combine(Path.GetDirectoryName(full), name);
                if (!File.Exists(library))
                    throw new InvalidDataException("The game build is incomplete: " + name + " must be beside jfg-native-boot.exe.");
                ValidatePe(library);
            }
            return full;
        }

        // Windows CRT command-line quoting. No shell parses these arguments.
        internal static string Quote(string value)
        {
            StringBuilder result = new StringBuilder("\"");
            int slashes = 0;
            foreach (char c in value)
            {
                if (c == '\\') { slashes++; continue; }
                if (c == '"') { result.Append('\\', slashes * 2 + 1); result.Append(c); }
                else { result.Append('\\', slashes); result.Append(c); }
                slashes = 0;
            }
            result.Append('\\', slashes * 2);
            result.Append('"');
            return result.ToString();
        }

        internal static ProcessStartInfo StartInfo(string runtime, string rom, string profile)
        {
            ProcessStartInfo info = new ProcessStartInfo();
            info.FileName = runtime;
            info.WorkingDirectory = Path.GetDirectoryName(runtime);
            info.UseShellExecute = false;
            info.CreateNoWindow = true;
            info.RedirectStandardOutput = true;
            info.RedirectStandardError = true;
            info.Arguments = "--rom " + Quote(rom) + " --save " + Quote(Path.Combine(profile, "jfg.flash")) +
                " --controller-pak " + Quote(Path.Combine(profile, "controller-1.pak")) + " --play";
            List<string> remove = new List<string>();
            foreach (string key in info.EnvironmentVariables.Keys)
                if (key.StartsWith("JFG_", StringComparison.OrdinalIgnoreCase)) remove.Add(key);
            foreach (string key in remove) info.EnvironmentVariables.Remove(key);
            if (File.Exists(ControllerProfile.FileName(profile))) {
                ControllerProfile.Load(profile);
                info.EnvironmentVariables["JFG_CONTROLLER_CONFIG"] = ControllerProfile.FileName(profile);
            }
            info.EnvironmentVariables["JFG_MASTER_VOLUME_CONFIG"] = AudioPreferences.FileName(profile);
            info.EnvironmentVariables["JFG_VIDEO_CONFIG"] = VideoPreferences.FileName(profile);
            return info;
        }

        internal static Settings LoadSettings(string directory)
        {
            try
            {
                string path = Path.Combine(directory, "launcher.json");
                if (!File.Exists(path) || new FileInfo(path).Length > 16384) return new Settings();
                using (FileStream stream = File.OpenRead(path))
                    return (Settings)new DataContractJsonSerializer(typeof(Settings)).ReadObject(stream) ?? new Settings();
            }
            catch (IOException) { return new Settings(); }
            catch (SerializationException) { return new Settings(); }
            catch (UnauthorizedAccessException) { return new Settings(); }
        }

        internal static void SaveSettings(string directory, Settings settings)
        {
            Directory.CreateDirectory(directory);
            string path = Path.Combine(directory, "launcher.json");
            string temporary = Path.Combine(directory, Guid.NewGuid().ToString("N") + ".tmp");
            try
            {
                using (FileStream output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                    new DataContractJsonSerializer(typeof(Settings)).WriteObject(output, settings);
                if (File.Exists(path)) File.Replace(temporary, path, null);
                else File.Move(temporary, path);
            }
            finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }

        internal static string FriendlyError(Exception error)
        {
            if (error is InvalidDataException) return error.Message;
            if (error is UnauthorizedAccessException) return "Access was denied. Choose readable local files and a writable user profile.";
            if (error is IOException) return "A local file could not be read or saved. Check permissions and close programs using it.";
            if (error is System.ComponentModel.Win32Exception) return "Windows could not start the selected program. Check Python for building, or the native build and its libraries for playing.";
            return "The launcher could not complete this operation. Check the selected files and the setup guide.";
        }
    }

    internal sealed class LauncherWindow : ApplicationWindow
    {
        private readonly TextBox runtime = new TextBox();
        private readonly TextBox rom = new TextBox();
        private readonly Label status = new Label();
        private readonly Button play = new Button();
        private readonly CheckBox navigation = new CheckBox();
        private readonly TrackBar volume = new TrackBar();
        private readonly CheckBox mute = new CheckBox();
        private readonly Label volumeValue = new Label();
        private readonly ProgressBar progress = new ProgressBar();
        private readonly List<Control> inputs = new List<Control>();
        private bool busy;
        private Process activeGame;
        private SupportSession activeSupport;
        private readonly Button freeze = new Button { Text = "Capture freeze", Size = new Size(145, 34), Enabled = false };

        internal LauncherWindow()
        {
            Text = "JFG Launcher Preview";
            ClientSize = new Size(760, 610);
            MinimumSize = new Size(776, 649);
            StartPosition = FormStartPosition.CenterScreen;
            Font = new Font("Segoe UI", 10);
            BackColor = Color.FromArgb(245, 247, 251);
            AutoScaleMode = AutoScaleMode.Dpi;
            TableLayoutPanel layout = new TableLayoutPanel();
            layout.Dock = DockStyle.Fill;
            layout.Padding = new Padding(24);
            layout.ColumnCount = 1;
            layout.RowCount = 11;
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 54));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 28));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 28));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 34));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 56));
            layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 8));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 132));
            Controls.Add(layout);
            Label title = new Label();
            title.Text = "Jet Force Gemini";
            title.Font = new Font("Segoe UI", 21, FontStyle.Bold);
            title.ForeColor = Color.FromArgb(25, 43, 70);
            title.Dock = DockStyle.Fill;
            layout.Controls.Add(title, 0, 0);
            Label summary = new Label();
            summary.Text = "ROM-to-play preview 0.4\nSelect your North American ROM. Setup builds your game locally.";
            summary.Dock = DockStyle.Fill;
            layout.Controls.Add(summary, 0, 1);
            AddLabel(layout, "1  Select your game ROM", 2);
            AddPicker(layout, rom, 3, "N64 ROM|*.z64;*.n64;*.v64|All files|*.*", "Select your North American retail ROM");
            AddLabel(layout, "2  Set up and build below, or select an existing build", 4);
            AddPicker(layout, runtime, 5, "Native game build|jfg-native-boot.exe", "Select the native game build");
            navigation.Text = "Navigation mod: full health, clear enemies, export maps (separate saves)";
            navigation.Dock = DockStyle.Fill;
            navigation.Checked = false;
            layout.Controls.Add(navigation, 0, 6);
            inputs.Add(navigation);
            status.Text = "First setup downloads missing tools and source. Windows may require a restart.\nFuture launches reuse your build. Saves stay in your user profile.";
            status.Dock = DockStyle.Fill;
            status.Padding = new Padding(0, 12, 0, 0);
            status.ForeColor = Color.FromArgb(64, 76, 92);
            layout.Controls.Add(status, 0, 8);
            progress.Dock = DockStyle.Fill;
            progress.Visible = false;
            progress.Style = ProgressBarStyle.Marquee;
            layout.Controls.Add(progress, 0, 9);
            FlowLayoutPanel actions = new FlowLayoutPanel();
            actions.Dock = DockStyle.Fill;
            actions.Padding = new Padding(0, 10, 0, 0);
            play.Text = "Launch game";
            play.Width = 145;
            play.Height = 34;
            play.BackColor = Color.FromArgb(30, 85, 155);
            play.ForeColor = Color.White;
            play.FlatStyle = FlatStyle.Flat;
            play.Click += async delegate { await Launch(); };
            actions.Controls.Add(play);
            inputs.Add(play);
            Button build = new Button();
            build.Text = "Set up and build";
            build.Size = new Size(145, 34);
            build.Click += async delegate { await Build(); };
            actions.Controls.Add(build);
            inputs.Add(build);
            Button guide = new Button();
            guide.Text = "Setup guide";
            guide.Size = new Size(120, 34);
            guide.Click += delegate { OpenGuide(); };
            actions.Controls.Add(guide);
            Button saves = new Button();
            saves.Text = "Open saves";
            saves.Size = new Size(120, 34);
            saves.Click += delegate {
                try
                {
                    Directory.CreateDirectory(LocalSetup.ProfileRoot);
                    Process.Start(new ProcessStartInfo("explorer.exe", LocalSetup.Quote(LocalSetup.ProfileRoot)) { UseShellExecute = false });
                }
                catch (Exception error) { status.Text = LocalSetup.FriendlyError(error); }
            };
            actions.Controls.Add(saves);
            Button controllers = new Button { Text = "Controllers", Size = new Size(145, 34) };
            controllers.Click += delegate { using (ControllerWindow window = new ControllerWindow(LocalSetup.ProfileRoot)) window.ShowDialog(this); };
            actions.Controls.Add(controllers); inputs.Add(controllers);
            Button report = new Button { Text = "Create support report", Size = new Size(205, 34) };
            report.Click += delegate {
                try {
                    using (SupportWindow window = new SupportWindow(SupportSession.Root, Path.Combine(FirstRun.Root, "support-exports"), activeSupport == null ? null : activeSupport.DirectoryPath)) window.ShowDialog(this);
                } catch (Exception error) { status.Text = LocalSetup.FriendlyError(error); }
            };
            actions.Controls.Add(report);
            Button maps = new Button { Text = "Live map", Size = new Size(120, 34) };
            maps.Click += delegate {
                try {using(var tool=NativeLiveTools.OpenWindow("map",LocalSetup.ProfileRoot,Handle)) {}}
                catch(Exception error){status.Text=LocalSetup.FriendlyError(error);}
            };
            actions.Controls.Add(maps);
            var inventoryButton=new Button{Text="Live inventory",Size=new Size(130,34)};
            inventoryButton.Click+=delegate {
                try {using(var tool=NativeLiveTools.OpenWindow("inventory",LocalSetup.ProfileRoot,Handle)) {}}
                catch(Exception error){status.Text=LocalSetup.FriendlyError(error);}
            };
            actions.Controls.Add(inventoryButton);
            layout.Controls.Add(actions, 0, 10);
            AudioPreferences audio;
            try { audio = AudioPreferences.Load(LocalSetup.ProfileRoot); }
            catch (Exception) {
                audio = new AudioPreferences { Muted = true };
                status.Text = "Saved audio settings could not be read. Choose a volume or uncheck Mute to save new settings.";
            }
            FlowLayoutPanel audioRow = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false };
            audioRow.Controls.Add(new Label { Text = "Volume", Width = 75, Padding = new Padding(0, 8, 0, 0) });
            volume.Minimum = 0; volume.Maximum = 100; volume.TickFrequency = 10;
            volume.SmallChange = 5; volume.LargeChange = 10; volume.Width = 330;
            volume.Value = audio.Volume; volume.AccessibleName = "Master volume";
            mute.Text = "Mute"; mute.Checked = audio.Muted; mute.AutoSize = true;
            mute.Padding = new Padding(0, 8, 0, 0); mute.AccessibleName = "Mute game audio";
            volumeValue.Width = 100; volumeValue.Padding = new Padding(0, 8, 0, 0);
            volumeValue.Text = audio.Muted ? "Muted (" + audio.Volume + "%)" : audio.Volume + "%";
            audioRow.Controls.Add(volume); audioRow.Controls.Add(volumeValue); audioRow.Controls.Add(mute);
            layout.Controls.Add(audioRow, 0, 7);
            volume.ValueChanged += delegate { SaveAudioPreference(); };
            mute.CheckedChanged += delegate { SaveAudioPreference(); };
            freeze.Click += async delegate {
                if (activeGame == null || activeSupport == null) return;
                freeze.Enabled = false;
                try {
                    bool captured = await activeSupport.CaptureFreeze(activeGame, activeGame.StartInfo.FileName);
                    status.Text = captured ? "Freeze captured. Create a support report for this session and attach the ZIP to your issue." : "Capture was incomplete. Create a support report; the remaining logs are still useful.";
                } catch (Exception error) { status.Text = LocalSetup.FriendlyError(error); }
                finally { freeze.Enabled = activeGame != null; }
            };
            actions.Controls.Add(freeze);
            Settings saved = LocalSetup.LoadSettings(LocalSetup.ProfileRoot);
            runtime.Text = LocalSetup.PreferredRuntime(AppDomain.CurrentDomain.BaseDirectory, saved.RuntimePath);
            rom.Text = saved.RomPath ?? "";
            FormClosing += delegate(object sender, FormClosingEventArgs e) {
                if (busy)
                {
                    e.Cancel = true;
                    MessageBox.Show(this, "Close the game or wait for the current build or ROM verification before closing the launcher.", Text,
                        MessageBoxButtons.OK, MessageBoxIcon.Information);
                }
            };
        }

        private void SaveAudioPreference()
        {
            volumeValue.Text = mute.Checked ? "Muted (" + volume.Value + "%)" : volume.Value + "%";
            try { new AudioPreferences { Volume = volume.Value, Muted = mute.Checked }.Save(LocalSetup.ProfileRoot); }
            catch (Exception error) { status.Text = "Could not save volume: " + LocalSetup.FriendlyError(error); }
        }

        private static void AddLabel(TableLayoutPanel layout, string text, int row)
        {
            Label label = new Label();
            label.Text = text;
            label.Dock = DockStyle.Fill;
            label.Font = new Font("Segoe UI", 10, FontStyle.Bold);
            layout.Controls.Add(label, 0, row);
        }

        private void AddPicker(TableLayoutPanel layout, TextBox field, int row, string filter, string title)
        {
            TableLayoutPanel picker = new TableLayoutPanel();
            picker.Dock = DockStyle.Fill;
            picker.ColumnCount = 2;
            picker.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            picker.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 95));
            picker.Margin = new Padding(0);
            field.Dock = DockStyle.Fill;
            Button browse = new Button();
            browse.Text = "Browse...";
            browse.Dock = DockStyle.Top;
            browse.Height = 29;
            browse.Click += delegate {
                using (OpenFileDialog dialog = new OpenFileDialog())
                {
                    dialog.Filter = filter;
                    dialog.Title = title;
                    dialog.CheckFileExists = true;
                    dialog.RestoreDirectory = true;
                    if (dialog.ShowDialog(this) == DialogResult.OK) field.Text = dialog.FileName;
                }
            };
            picker.Controls.Add(field, 0, 0);
            picker.Controls.Add(browse, 1, 0);
            inputs.Add(field);
            inputs.Add(browse);
            layout.Controls.Add(picker, 0, row);
        }

        private void OpenGuide()
        {
            try { Process.Start(new ProcessStartInfo(LocalSetup.Guide) { UseShellExecute = true }); }
            catch (Exception error) { status.Text = LocalSetup.FriendlyError(error); }
        }

        private async Task Build()
        {
            if (busy) return;
            busy = true;
            foreach (Control control in inputs) control.Enabled = false;
            progress.Visible = true;
            FileStream checkedRom = null;
            SupportSession support = null;
            try
            {
                support = new SupportSession(SupportSession.Root, "setup");
                activeSupport = support;
                await Task.Run(delegate { try { support.SystemDetails(); } catch (IOException) { support.Write("diagnostic=system-partial"); } catch (UnauthorizedAccessException) { support.Write("diagnostic=system-partial"); } });
                string selectedRom = LocalSetup.FullPath(rom.Text);
                status.Text = "Verifying your ROM locally...";
                checkedRom = await Task.Run(delegate { return LocalSetup.OpenVerifiedRom(selectedRom); });
                LocalSetup.SaveSettings(LocalSetup.ProfileRoot, new Settings { RuntimePath = runtime.Text, RomPath = selectedRom });
                if (MessageBox.Show(this, "Setup reuses installed tools and downloads source and dependencies. " +
                    "Missing Git, Python and Visual Studio C++ tools can require several GB, administrator approval and a Windows restart. " +
                    "Your ROM and generated game stay on this computer. Continue?", Text,
                    MessageBoxButtons.OKCancel, MessageBoxIcon.Information) != DialogResult.OK) return;
                string built = null;
                string failure = null;
                using (Process process = new Process())
                {
                    process.StartInfo = FirstRun.StartInfo(selectedRom, Path.Combine(FirstRun.Root, "setup", BuildInfo.SourceCommit));
                    process.OutputDataReceived += delegate(object sender, DataReceivedEventArgs e) {
                        if (String.IsNullOrWhiteSpace(e.Data)) return;
                        support.SetupLine(e.Data);
                        if (e.Data.StartsWith("Built: ", StringComparison.Ordinal)) built = e.Data.Substring(7);
                        string message = e.Data.Length > 300 ? e.Data.Substring(0, 300) : e.Data;
                        BeginInvoke((Action)delegate { status.Text = message + "\nSetup and compilation can take a while. Progress is saved in setup.log."; });
                    };
                    process.ErrorDataReceived += delegate(object sender, DataReceivedEventArgs e) {
                        support.SetupLine(e.Data);
                        if (!String.IsNullOrWhiteSpace(e.Data)) failure = e.Data.Length > 400 ? e.Data.Substring(0, 400) : e.Data;
                    };
                    new AudioPreferences { Volume = volume.Value, Muted = mute.Checked }.Save(LocalSetup.ProfileRoot);
                    if (!process.Start()) throw new IOException();
                    process.BeginOutputReadLine();
                    process.BeginErrorReadLine();
                    status.Text = "Building locally. Dependencies may be downloaded on the first run.\nThis can take several minutes.";
                    await Task.Run(delegate { process.WaitForExit(); });
                    support.Exit(process.ExitCode);
                    if (process.ExitCode == 3010)
                    {
                        support.Write("stage=restart-required");
                        status.Text = "Restart Windows, reopen the launcher and click Set up and build again.\nYour ROM selection and saves are preserved.";
                        return;
                    }
                    if (process.ExitCode != 0 || built == null)
                    {
                        status.Text = "Build stopped. " + (failure ?? "Open the setup guide or read " + Path.Combine(FirstRun.Root, "setup.log") + ".");
                        return;
                    }
                }
                runtime.Text = LocalSetup.ValidateRuntime(built);
                LocalSetup.SaveSettings(LocalSetup.ProfileRoot, new Settings { RuntimePath = runtime.Text, RomPath = selectedRom });
                support.Write("stage=ready");
                status.Text = "Build ready. Click Launch game to play.\nThe generated game build stays on your computer.";
            }
            catch (Exception error) { if (support != null) support.Error(error); status.Text = LocalSetup.FriendlyError(error); }
            finally
            {
                activeSupport = null; activeGame = null; freeze.Enabled = false;
                if (support != null) support.Finish();
                if (checkedRom != null) checkedRom.Dispose();
                busy = false;
                progress.Visible = false;
                foreach (Control control in inputs) control.Enabled = true;
            }
        }

        protected override void WndProc(ref Message message) {
            if(message.Msg==0x8017) {try{using(var tool=NativeLiveTools.OpenWindow("inventory",LocalSetup.ProfileRoot,Handle)) {}}catch(IOException error){status.Text=error.Message;}return;}
            base.WndProc(ref message);
        }
        private void ShowNavigationMap(string path)
        {
            using(var tool=NativeLiveTools.OpenWindow("map",LocalSetup.ProfileRoot,Handle)) {}
        }

        private async Task Launch()
        {
            if (busy) return;
            busy = true;
            foreach (Control control in inputs) control.Enabled = false;
            progress.Visible = true;
            FileStream checkedRom = null;
            SupportSession support = null;
            try
            {
                support = new SupportSession(SupportSession.Root, "launch");
                activeSupport = support;
                await Task.Run(delegate { try { support.SystemDetails(); } catch (IOException) { support.Write("diagnostic=system-partial"); } catch (UnauthorizedAccessException) { support.Write("diagnostic=system-partial"); } });
                string game = LocalSetup.ValidateRuntime(runtime.Text);
                bool captureAvailable = await Task.Run(delegate { try { return support.RuntimeDetails(game, LocalSetup.ProfileRoot); } catch (IOException) { return false; } catch (UnauthorizedAccessException) { return false; } });
                string selectedRom = LocalSetup.FullPath(rom.Text);
                status.Text = "Verifying your ROM locally...";
                checkedRom = await Task.Run(delegate { return LocalSetup.OpenVerifiedRom(selectedRom); });
                LocalSetup.SaveSettings(LocalSetup.ProfileRoot, new Settings { RuntimePath = game, RomPath = selectedRom });
                using (Process process = new Process())
                {
                    process.StartInfo = navigation.Checked
                        ? LocalSetup.NavigationStartInfo(game, selectedRom, LocalSetup.ProfileRoot)
                        : LocalSetup.LiveToolsStartInfo(game, selectedRom, LocalSetup.ProfileRoot);
                    support.Write(navigation.Checked ? "mod=navigation-enabled" : "mod=disabled");
                    process.StartInfo.EnvironmentVariables["JFG_SUPPORT_LOG"] = support.NativePath;
                    new AudioPreferences { Volume = volume.Value, Muted = mute.Checked }.Save(LocalSetup.ProfileRoot);
                    if (!process.Start()) throw new IOException();
                    InventorySession.Record(navigation.Checked?process.StartInfo.EnvironmentVariables["JFG_MOD_OUTPUT"]:process.StartInfo.EnvironmentVariables["JFG_LIVE_OUTPUT"],process);
                    InventorySession.Record(LocalSetup.ProfileRoot,process);
                    File.WriteAllText(Path.Combine(LocalSetup.ProfileRoot,"frontend-map-session.txt"),navigation.Checked?process.StartInfo.EnvironmentVariables["JFG_MOD_OUTPUT"]:process.StartInfo.EnvironmentVariables["JFG_LIVE_OUTPUT"]);
                    File.WriteAllText(Path.Combine(LocalSetup.ProfileRoot,"frontend-rom.txt"),selectedRom);
                    if (navigation.Checked) ShowNavigationMap(process.StartInfo.EnvironmentVariables["JFG_MOD_OUTPUT"]);
                    support.Write("stage=started");
                    activeGame = process; freeze.Enabled = captureAvailable;
                    Task stdout = support.Drain(process.StandardOutput), stderr = support.Drain(process.StandardError);
                    progress.Visible = false;
                    status.Text = navigation.Checked
                        ? "Navigation mod started. Full health and automatic enemy clearing are enabled.\nMaps are exported locally. This run uses separate mod saves."
                        : "Game started. Close the game to return here.\nYour saves remain in your Windows user profile.";
                    await Task.Run(delegate { process.WaitForExit(); });
                    await Task.WhenAll(stdout, stderr);
                    activeGame = null; freeze.Enabled = false;
                    support.Exit(process.ExitCode);
                    status.Text = process.ExitCode == 0 ? "Game closed. Your save profile is ready for next time." :
                        "The game exited with code " + process.ExitCode.ToString() + ". Click Create support report and attach the ZIP to a GitHub issue.";
                }
            }
            catch (Exception error) { if (support != null) support.Error(error); status.Text = LocalSetup.FriendlyError(error); }
            finally
            {
                activeSupport = null; activeGame = null; freeze.Enabled = false;
                if (support != null) support.Finish();
                if (checkedRom != null) checkedRom.Dispose();
                busy = false;
                progress.Visible = false;
                foreach (Control control in inputs) control.Enabled = true;
            }
        }
    }

    internal static class Program
    {
        [STAThread]
        private static void Main(string[] args)
        {
            if (FrontendBridge.TryRun(args)) return;
            if (args.Length == 2 && args[0] == "--map-view") {
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                using(var tool=NativeLiveTools.OpenWindow("map",LocalSetup.FullPath(args[1]),IntPtr.Zero))tool.WaitForExit();
                return;
            }
            bool owner;
            using (Mutex instance = new Mutex(true, @"Local\JFGRecompLauncher", out owner))
            {
                if (!owner)
                {
                    MessageBox.Show("The JFG launcher is already open.", "JFG Launcher Preview");
                    return;
                }
                try
                {
                    Application.EnableVisualStyles();
                    Application.SetCompatibleTextRenderingDefault(false);
                    Application.Run(new LauncherWindow());
                }
                catch (Exception error)
                {
                    MessageBox.Show(LocalSetup.FriendlyError(error), "JFG Launcher Preview", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
                finally { instance.ReleaseMutex(); }
            }
        }
    }
}
