using System.Diagnostics;
using System.IO.Ports;
using System.Reflection;
using System.Text.Json;

namespace SG98Updater;

internal static class Program
{
    private const string DefaultBaud = "460800";
    private const string EsptoolResource = "Assets.esptool.exe";
    private const string ManifestResource = "Assets.sg98-flash-manifest.json";

    private static int Main(string[] args)
    {
        Console.Title = "SG98 Updater";
        Console.WriteLine("SG98 Firmware Updater");
        Console.WriteLine("---------------------");
        Console.WriteLine("Use USB-C only during update. Keep +24V disconnected.");
        Console.WriteLine();

        string? selectedPort = null;

        try
        {
            string port = ResolvePort(args);
            selectedPort = port;
            string workDir = Path.Combine(Path.GetTempPath(), "SG98Updater");
            Directory.CreateDirectory(workDir);

            FlashManifest manifest = LoadManifest();
            List<ExtractedSegment> segments = ExtractFlashSegments(manifest, workDir);

            Console.WriteLine($"Bundle target: {DescribeBundle(manifest)}");
            Console.WriteLine($"Port: {port}");
            Console.WriteLine("Flashing controller image set...");
            Console.WriteLine();

            string imageArgs = string.Join(" ", segments.Select(static segment => $"{segment.Offset} \"{segment.Path}\""));
            string flasherArgs =
                $"--chip esp32s3 --port {port} --baud {DefaultBaud} --before default_reset --after hard_reset " +
                $"write_flash -z --flash_size detect {imageArgs}";

            var flasher = ResolveFlasher(workDir);
            int exitCode = RunProcess(flasher.FileName, flasher.ArgumentsPrefix + flasherArgs);
            if (exitCode != 0)
            {
                Console.ForegroundColor = ConsoleColor.Red;
                Console.WriteLine();
                Console.WriteLine($"Update failed with exit code {exitCode}.");
                Console.ResetColor();
                PrintTroubleshootingHint(selectedPort);
                PauseIfInteractive();
                return exitCode;
            }

            Console.ForegroundColor = ConsoleColor.Green;
            Console.WriteLine();
            Console.WriteLine("Update complete.");
            Console.ResetColor();
            PauseIfInteractive();
            return 0;
        }
        catch (Exception ex)
        {
            Console.ForegroundColor = ConsoleColor.Red;
            Console.WriteLine();
            Console.WriteLine($"Error: {ex.Message}");
            Console.ResetColor();
            PrintTroubleshootingHint(selectedPort);
            PauseIfInteractive();
            return 1;
        }
    }

    private static FlashManifest LoadManifest()
    {
        using Stream? stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(ManifestResource);
        if (stream is null)
        {
            throw new InvalidOperationException($"Embedded resource not found: {ManifestResource}");
        }

        using StreamReader reader = new(stream);
        FlashManifest? manifest = JsonSerializer.Deserialize<FlashManifest>(
            reader.ReadToEnd(),
            new JsonSerializerOptions
            {
                PropertyNameCaseInsensitive = true,
            });

        if (manifest is null || manifest.Segments.Count == 0)
        {
            throw new InvalidOperationException("Updater manifest is missing flash segment data.");
        }

        foreach (FlashSegment segment in manifest.Segments)
        {
            if (string.IsNullOrWhiteSpace(segment.Offset) || string.IsNullOrWhiteSpace(segment.FileName))
            {
                throw new InvalidOperationException("Updater manifest contains an incomplete flash segment entry.");
            }
        }

        manifest.Segments = manifest.Segments
            .OrderBy(static segment => ParseOffset(segment.Offset!))
            .ToList();
        return manifest;
    }

    private static List<ExtractedSegment> ExtractFlashSegments(FlashManifest manifest, string workDir)
    {
        List<ExtractedSegment> segments = new();
        foreach (FlashSegment segment in manifest.Segments)
        {
            string safeFileName = Path.GetFileName(segment.FileName!);
            if (!string.Equals(safeFileName, segment.FileName, StringComparison.Ordinal))
            {
                throw new InvalidOperationException($"Invalid asset name in updater manifest: {segment.FileName}");
            }

            string resourceName = $"Assets.{safeFileName}";
            string extractedPath = ExtractResource(resourceName, Path.Combine(workDir, safeFileName));
            segments.Add(new ExtractedSegment(segment.Offset!, extractedPath, segment.Kind ?? "image"));
        }

        return segments;
    }

    private static (string FileName, string ArgumentsPrefix) ResolveFlasher(string workDir)
    {
        if (OperatingSystem.IsWindows())
        {
            string esptoolPath = ExtractResource(EsptoolResource, Path.Combine(workDir, "esptool.exe"));
            return (esptoolPath, string.Empty);
        }

        if (CommandSucceeds("python3", "-c \"import esptool\""))
        {
            return ("python3", "-m esptool ");
        }

        if (CommandSucceeds("python", "-c \"import esptool\""))
        {
            return ("python", "-m esptool ");
        }

        if (CommandSucceeds("esptool.py", "version"))
        {
            return ("esptool.py", string.Empty);
        }

        if (CommandSucceeds("esptool", "version"))
        {
            return ("esptool", string.Empty);
        }

        throw new InvalidOperationException(
            "esptool is not available on this system. Install it with: python3 -m pip install --user esptool");
    }

    private static string ResolvePort(string[] args)
    {
        string? explicitPort = null;
        for (int i = 0; i < args.Length; i++)
        {
            if (string.Equals(args[i], "--port", StringComparison.OrdinalIgnoreCase) && i + 1 < args.Length)
            {
                explicitPort = args[i + 1];
                break;
            }
        }

        if (!string.IsNullOrWhiteSpace(explicitPort))
        {
            return explicitPort!;
        }

        string[] ports = GetInteractivePorts();

        if (ports.Length == 0)
        {
            throw new InvalidOperationException($"No {PortLabelPlural()} found. Connect the board over USB-C and try again.");
        }

        string[] likelyPorts = ports
            .Where(IsLikelyUsbFlashingPort)
            .ToArray();

        if (likelyPorts.Length == 1)
        {
            Console.WriteLine($"Using detected USB serial port: {likelyPorts[0]}");
            Console.WriteLine();
            return likelyPorts[0];
        }

        if (ports.Length == 1)
        {
            return ports[0];
        }

        Console.WriteLine($"Available {PortLabelPlural()}:");
        for (int i = 0; i < ports.Length; i++)
        {
            Console.WriteLine($"  {i + 1}. {ports[i]}{PortHintSuffix(ports[i])}");
        }
        Console.WriteLine();
        if (likelyPorts.Length > 0)
        {
            Console.WriteLine("Ports marked [USB] are the best SG98 flashing candidates.");
            Console.WriteLine();
        }
        Console.Write($"Choose {PortLabelSingular()} number: ");
        string? input = Console.ReadLine();
        if (!int.TryParse(input, out int selected) || selected < 1 || selected > ports.Length)
        {
            throw new InvalidOperationException($"Invalid {PortLabelSingular()} selection.");
        }

        return ports[selected - 1];
    }

    private static string[] GetInteractivePorts()
    {
        IEnumerable<string> ports = SerialPort.GetPortNames()
            .Distinct(StringComparer.OrdinalIgnoreCase);

        if (OperatingSystem.IsMacOS())
        {
            string[] calloutPorts = ports
                .Where(static port => port.StartsWith("/dev/cu.", StringComparison.OrdinalIgnoreCase))
                .ToArray();

            if (calloutPorts.Length > 0)
            {
                ports = calloutPorts;
            }
        }

        return ports
            .OrderBy(PortPriority)
            .ThenBy(static p => p, StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    private static string ExtractResource(string resourceName, string targetPath)
    {
        using Stream? stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(resourceName);
        if (stream is null)
        {
            throw new InvalidOperationException($"Embedded resource not found: {resourceName}");
        }

        using FileStream file = File.Create(targetPath);
        stream.CopyTo(file);
        return targetPath;
    }

    private static bool CommandSucceeds(string fileName, string arguments)
    {
        try
        {
            using Process process = new();
            process.StartInfo.FileName = fileName;
            process.StartInfo.Arguments = arguments;
            process.StartInfo.UseShellExecute = false;
            process.StartInfo.RedirectStandardOutput = true;
            process.StartInfo.RedirectStandardError = true;
            process.StartInfo.CreateNoWindow = true;

            process.Start();
            process.WaitForExit();
            return process.ExitCode == 0;
        }
        catch
        {
            return false;
        }
    }

    private static int RunProcess(string fileName, string arguments)
    {
        using Process process = new();
        process.StartInfo.FileName = fileName;
        process.StartInfo.Arguments = arguments;
        process.StartInfo.UseShellExecute = false;
        process.StartInfo.RedirectStandardOutput = true;
        process.StartInfo.RedirectStandardError = true;
        process.StartInfo.CreateNoWindow = false;

        process.OutputDataReceived += (_, e) =>
        {
            if (!string.IsNullOrEmpty(e.Data))
            {
                Console.WriteLine(e.Data);
            }
        };
        process.ErrorDataReceived += (_, e) =>
        {
            if (!string.IsNullOrEmpty(e.Data))
            {
                Console.Error.WriteLine(e.Data);
            }
        };

        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        process.WaitForExit();
        return process.ExitCode;
    }

    private static string DescribeBundle(FlashManifest manifest)
    {
        string env = string.IsNullOrWhiteSpace(manifest.EnvName) ? "unknown-env" : manifest.EnvName!;
        string board = string.IsNullOrWhiteSpace(manifest.BoardName) ? env : manifest.BoardName!;
        return $"{env} [{board}]";
    }

    private static long ParseOffset(string value)
    {
        return Convert.ToInt64(value, 16);
    }

    private static int PortPriority(string port)
    {
        if (IsLikelyUsbFlashingPort(port))
        {
            return 0;
        }

        if (IsUsuallyWrongPort(port))
        {
            return 2;
        }

        return 1;
    }

    private static bool IsLikelyUsbFlashingPort(string port)
    {
        string normalized = port.ToLowerInvariant();

        return normalized.StartsWith("com", StringComparison.OrdinalIgnoreCase)
            || normalized.Contains("usbmodem", StringComparison.Ordinal)
            || normalized.Contains("usbserial", StringComparison.Ordinal)
            || normalized.Contains("wchusbserial", StringComparison.Ordinal)
            || normalized.Contains("slab_usbto", StringComparison.Ordinal)
            || normalized.Contains("ttyacm", StringComparison.Ordinal)
            || normalized.Contains("ttyusb", StringComparison.Ordinal);
    }

    private static bool IsUsuallyWrongPort(string port)
    {
        string normalized = port.ToLowerInvariant();
        return normalized.Contains("bluetooth", StringComparison.Ordinal)
            || normalized.Contains("debug-console", StringComparison.Ordinal);
    }

    private static string PortHintSuffix(string port)
    {
        if (IsLikelyUsbFlashingPort(port))
        {
            return "  [USB]";
        }

        if (IsUsuallyWrongPort(port))
        {
            return "  [skip]";
        }

        return string.Empty;
    }

    private static string PortLabelPlural() => OperatingSystem.IsWindows() ? "COM ports" : "serial ports";

    private static string PortLabelSingular() => OperatingSystem.IsWindows() ? "COM port" : "serial port";

    private static void PrintTroubleshootingHint(string? selectedPort)
    {
        Console.WriteLine();
        if (OperatingSystem.IsWindows())
        {
            Console.WriteLine("If the board does not appear as a COM port, install the Espressif ESP32-C3/S3 USB driver support:");
            Console.WriteLine("https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/jtag-debugging/configure-builtin-jtag.html");
            Console.WriteLine("If upload still fails, hold BOOT0, short-press EN, then retry the update.");
            return;
        }

        if (!string.IsNullOrWhiteSpace(selectedPort) && !IsLikelyUsbFlashingPort(selectedPort))
        {
            Console.WriteLine($"The selected port {selectedPort} does not look like the ESP32-S3 USB flashing port.");
        }

        Console.WriteLine("Prefer a /dev/cu.usbmodem* or /dev/cu.usbserial* device for the SG98 board.");
        Console.WriteLine("If upload still fails, hold BOOT0, short-press EN, then retry the update.");
        Console.WriteLine("If flashing fails because esptool is missing, install it with:");
        Console.WriteLine("python3 -m pip install --user esptool");
    }

    private static void PauseIfInteractive()
    {
        if (Environment.UserInteractive)
        {
            Console.WriteLine();
            Console.Write("Press Enter to close...");
            Console.ReadLine();
        }
    }

    private sealed class FlashManifest
    {
        public string? EnvName { get; set; }
        public string? BoardName { get; set; }
        public string? ApplicationOffset { get; set; }
        public List<FlashSegment> Segments { get; set; } = [];
    }

    private sealed class FlashSegment
    {
        public string? Offset { get; set; }
        public string? FileName { get; set; }
        public string? SourceName { get; set; }
        public string? Kind { get; set; }
    }

    private sealed record ExtractedSegment(string Offset, string Path, string Kind);
}
