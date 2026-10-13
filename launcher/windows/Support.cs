using System;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace JfgLauncher
{
    // Shareable diagnostics have an explicit grammar. Raw console output, exception
    // messages, paths, ROM bytes, saves and memory dumps never enter a report.
    internal sealed partial class SupportSession
    {
        internal const int MaximumBytes = 65536;
        internal readonly string DirectoryPath;
        private readonly object gate = new object();
        private int omitted;
        internal string NativePath { get { return Path.Combine(DirectoryPath, "native.log"); } }
        internal static string Root { get { return Path.Combine(FirstRun.Root, "reports"); } }
        internal static readonly Regex SafeLine = new Regex(
            @"\A(?:stage=(?:setup|launch|verify-rom|install-tools|download-source|build-game|ready|restart-required|started|closed|failed)|exit=0x[0-9a-f]{8}|exception=0x[0-9a-f]{8}|runtime_sha256=[0-9a-f]{64}|source=[0-9a-f]{40}|version=[0-9a-z.-]{1,40}|os=[0-9.]{1,40}|utc=[0-9TZ:.+-]{1,40}|omitted=[0-9]{1,10}|mod=(?:disabled|navigation-enabled)|native=(?:boot|rom-ready|renderer-ready|running|closed|controller-connected|controller-disconnected|controller-invalid)|(?:native_exit|native_exception)=0x[0-9a-f]{8}|failure=[a-z0-9-]{1,64}/[a-z0-9-]{1,64}|architecture=x64|cpu_threads=[0-9]{1,4}|windows_build=[0-9.]{1,30}|gpu=[0-7]/[0-9a-f]{4}/[0-9a-f]{4}/[0-9.]{1,40}|build_source=[0-9a-f]{40}|build_dirty=[01]|build_(?:tree|runtime|dependencies|symbols|capture)_sha256=[0-9a-f]{64}|build_symbols_id=[0-9a-f]{32}-[0-9a-f]{8}|build_configuration=release|build_(?:identity|symbols)=unavailable|controller_(?:version|device|stick|deadzone|threshold|trigger|invert_x|invert_y|map[0-9]{1,2})=-?[0-9]{1,5}|setup_(?:step|error)=[a-z0-9-]{1,64}|setup_exit=0x[0-9a-f]{8}|compiler_error=(?:c|lnk|msb)[0-9]{4}|diagnostic=(?:controller-unavailable|gpu-unavailable|system-partial|stack-partial)|capture=(?:requested|complete|failed|timeout)|capture_error=0x[0-9a-f]{8}|capture_scope=(?:direct|child)|snapshot=(?:crash|hang)-v1|elapsed_ms=[0-9]{1,20}|breadcrumb=[0-9]{1,20}/[0-9]{1,20}/[0-9]{1,20}/[0-9]{1,20}|frame=[0-9]{1,10}/[0-9]{1,2}/[a-z0-9_.-]{1,96}/[0-9a-f]{8}/[0-9a-f]{8}/[0-9a-f]{8,16})\z",
            RegexOptions.CultureInvariant);

        internal SupportSession(string root, string stage)
        {
            Directory.CreateDirectory(root);
            DirectoryPath = Path.Combine(root, DateTime.UtcNow.ToString("yyyyMMddTHHmmssfff", CultureInfo.InvariantCulture) + "-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(DirectoryPath);
            Write("version=" + ((AssemblyInformationalVersionAttribute)Attribute.GetCustomAttribute(
                Assembly.GetExecutingAssembly(), typeof(AssemblyInformationalVersionAttribute))).InformationalVersion);
            Write("source=" + BuildInfo.SourceCommit);
            Write("os=" + Environment.OSVersion.Version.ToString());
            Write("utc=" + DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture));
            Write("stage=" + stage);
            Prune(root);
        }

        internal void Write(string line)
        {
            if (line == null || !SafeLine.IsMatch(line)) return;
            lock (gate)
            {
                string path = Path.Combine(DirectoryPath, "launcher.log");
                try
                {
                    using (FileStream file = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite))
                    {
                        byte[] bytes = Encoding.ASCII.GetBytes(line + "\n");
                        if (file.Length + bytes.Length > MaximumBytes) return;
                        file.Write(bytes, 0, bytes.Length);
                        file.Flush(true);
                    }
                }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
        }

        internal void Error(Exception error) { Write("exception=0x" + error.HResult.ToString("x8", CultureInfo.InvariantCulture)); Write("stage=failed"); }
        internal void Exit(int code) { Write("exit=0x" + code.ToString("x8", CultureInfo.InvariantCulture)); Write("stage=" + (code == 0 ? "closed" : code == 3010 ? "restart-required" : "failed")); }

        internal void ConsoleLine(string line)
        {
            // Compatibility with older native builds; retain the actual child crash code.
            const string prefix = "native boot child rejected: exit=0x";
            if (line.StartsWith(prefix, StringComparison.Ordinal) && Regex.IsMatch(line.Substring(prefix.Length), @"\A[0-9a-f]{8}\z"))
                Write("native_exit=0x" + line.Substring(prefix.Length));
            else if (line.StartsWith("JFG-SUPPORT ", StringComparison.Ordinal) && SafeLine.IsMatch(line.Substring(12)))
                Write(line.Substring(12));
            else if (line.Length != 0) System.Threading.Interlocked.Increment(ref omitted);
        }

        internal Task Drain(StreamReader reader)
        {
            // Read fixed chunks, including when a process emits an enormous unterminated line.
            return Task.Run(delegate {
                char[] buffer = new char[512];
                StringBuilder line = new StringBuilder();
                bool discard = false;
                int count;
                while ((count = reader.Read(buffer, 0, buffer.Length)) != 0)
                    for (int i = 0; i < count; ++i)
                    {
                        char c = buffer[i];
                        if (c == '\n') { if (!discard) ConsoleLine(line.ToString().TrimEnd('\r')); line.Clear(); discard = false; }
                        else if (!discard) { if (line.Length >= 256) { discard = true; line.Clear(); System.Threading.Interlocked.Increment(ref omitted); } else line.Append(c); }
                    }
                if (!discard && line.Length > 0) ConsoleLine(line.ToString().TrimEnd('\r'));
            });
        }

        internal void Finish() { Write("omitted=" + Math.Max(0, omitted).ToString(CultureInfo.InvariantCulture)); }

        internal static string SanitizeFile(string path)
        {
            if (!File.Exists(path) || (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) return "";
            using (FileStream file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            {
                byte[] data = new byte[MaximumBytes];
                int count = 0, read;
                while (count < data.Length && (read = file.Read(data, count, data.Length - count)) > 0) count += read;
                StringBuilder safe = new StringBuilder();
                foreach (string raw in Encoding.ASCII.GetString(data, 0, count).Split('\n'))
                {
                    string line = raw.TrimEnd('\r');
                    if (line.Length <= 256 && SafeLine.IsMatch(line)) safe.Append(line).Append('\n');
                }
                return safe.ToString();
            }
        }

        internal static string Export(string root, string destinationDirectory)
        {
            var choices = Sessions(root);
            if (choices.Count == 0) throw new InvalidDataException("No support session is available yet.");
            SupportChoice selected = choices.Find(delegate(SupportChoice item) { return item.Failed; }) ?? choices[0];
            return ExportSelected(root, selected.Path, destinationDirectory);
        }

        internal static string ExportSelected(string root, string selected, string destinationDirectory)
        {
            root = Path.GetFullPath(root); selected = Path.GetFullPath(selected);
            if (!String.Equals(Path.GetDirectoryName(selected), root.TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase) || !Owned(selected))
                throw new InvalidDataException("Select an existing support session.");
            Directory.CreateDirectory(destinationDirectory);
            string destination = Path.Combine(destinationDirectory, "JFG-support-" + Guid.NewGuid().ToString("N") + ".zip");
            using (FileStream output = new FileStream(destination, FileMode.CreateNew))
            using (ZipArchive zip = new ZipArchive(output, ZipArchiveMode.Create))
            {
                foreach (string name in Files)
                    using (StreamWriter writer = new StreamWriter(zip.CreateEntry(name).Open(), new UTF8Encoding(false)))
                        writer.Write(SanitizeFile(Path.Combine(selected, name)));
                using (StreamWriter writer = new StreamWriter(zip.CreateEntry("README.txt").Open(), new UTF8Encoding(false)))
                    writer.Write("Attach this ZIP to an issue at https://github.com/ravenx444/RGO-Recomp/issues\n" +
                        "Describe what you were doing, the level/menu, expected behavior, and steps to reproduce.\n" +
                        "No automatic upload occurs. Report: selected session, build/symbol identity, GPU driver IDs, settings, setup codes, recent progress and available crash/freeze stacks.\n" +
                        "It excludes ROMs, saves, paths, raw console logs and memory dumps. Inspect the text files before sharing. Empty files mean the diagnostic was unavailable. Stack snapshots are not Windows memory dumps.\n");
            }
            return destination;
        }
    }
}
