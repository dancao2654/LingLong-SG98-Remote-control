using System.Diagnostics;
using System.IO.Ports;
using System.Reflection;
using System.Text;
using System.Text.Json;

namespace SG98Updater;

internal static class Program
{
    private const string DefaultBaud = "460800";
    private const string EsptoolResource = "Assets.esptool.exe";
    private const string FlashManifestResource = "Assets.sg98-flash-manifest.json";
    private static readonly JsonSerializerOptions JsonOptions = new() { PropertyNameCaseInsensitive = true };

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
            if (HelpRequested(args))
            {
                PrintUsage();
                return 0;
            }

            if (ListPortsRequested(args))
            {
                string listWorkDir = Path.Combine(Path.GetTempPath(), "SG98Updater");
                Directory.CreateDirectory(listWorkDir);
                PrintPortList(listWorkDir);
                return 0;
            }

            string port = ResolvePort(args);
            UpdateMode requestedMode = ResolveUpdateMode(args);
            bool forceMode = ForceModeRequested(args);
            if (forceMode && requestedMode == UpdateMode.Application)
            {
                throw new InvalidOperationException("--force cannot be combined with --app-only.");
            }
            if (forceMode)
            {
                requestedMode = UpdateMode.Full;
            }
            selectedPort = port;
            string workDir = Path.Combine(Path.GetTempPath(), "SG98Updater");
            Directory.CreateDirectory(workDir);
            FlashManifest manifest = LoadFlashManifest();
            IReadOnlyList<ExtractedFlashSegment> flashSegments = ExtractFlashSegments(manifest, workDir);
            DeviceIdentity? device = forceMode ? null : TryReadDeviceIdentity(port);
            IReadOnlyList<ExtractedFlashSegment> selectedSegments =
                SelectFlashSegments(manifest, flashSegments, device, requestedMode);

            Console.WriteLine($"Port: {port}");
            Console.WriteLine($"Bundle: {manifest.EnvName}");
            if (!string.IsNullOrWhiteSpace(manifest.Version))
            {
                Console.WriteLine($"Version: v{manifest.Version} {manifest.GitSha}".TrimEnd());
            }
            if (device is not null)
            {
                Console.WriteLine($"Detected: {device.Role} v{device.FirmwareVersion} {device.BuildEnv}".TrimEnd());
            }
            else if (forceMode)
            {
                Console.WriteLine("Force mode: USB-C identity probe skipped.");
            }
            else
            {
                Console.WriteLine("Detected: unavailable; using full-flash fallback.");
            }
            Console.WriteLine($"Update mode: {UpdateModeDescription(selectedSegments, flashSegments)}");
            Console.WriteLine(selectedSegments.Count == flashSegments.Count ? "Flashing full image set..." : "Flashing application firmware...");
            foreach (ExtractedFlashSegment segment in selectedSegments)
            {
                Console.WriteLine($"  {segment.Offset} -> {segment.FileName}");
            }
            Console.WriteLine();

            string flasherArgs = BuildWriteFlashArgs(port, selectedSegments);
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

    private static FlashManifest LoadFlashManifest()
    {
        using Stream? stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(FlashManifestResource);
        if (stream is null)
        {
            throw new InvalidOperationException($"Embedded resource not found: {FlashManifestResource}");
        }

        using StreamReader reader = new(stream);
        FlashManifest? manifest = JsonSerializer.Deserialize<FlashManifest>(reader.ReadToEnd(), JsonOptions);
        if (manifest is null || manifest.Segments.Count == 0)
        {
            throw new InvalidOperationException("Embedded flash manifest is missing or empty");
        }

        return manifest;
    }

    private static IReadOnlyList<ExtractedFlashSegment> ExtractFlashSegments(FlashManifest manifest, string workDir)
    {
        List<ExtractedFlashSegment> extracted = [];
        foreach (FlashSegment segment in manifest.Segments.OrderBy(ParseOffset))
        {
            if (string.IsNullOrWhiteSpace(segment.FileName) || string.IsNullOrWhiteSpace(segment.Offset))
            {
                throw new InvalidOperationException("Flash manifest contains an invalid segment entry");
            }

            string segmentPath = ExtractResource(
                $"Assets.{segment.FileName}",
                Path.Combine(workDir, segment.FileName));
            extracted.Add(new ExtractedFlashSegment(segment.Offset, segment.FileName, segmentPath, segment.Kind ?? string.Empty));
        }

        return extracted;
    }

    private static bool HelpRequested(string[] args)
    {
        return args.Any(arg =>
            string.Equals(arg, "-h", StringComparison.OrdinalIgnoreCase)
            || string.Equals(arg, "--help", StringComparison.OrdinalIgnoreCase)
            || string.Equals(arg, "/?", StringComparison.OrdinalIgnoreCase));
    }

    private static bool ForceModeRequested(string[] args)
    {
        if (args.Any(arg =>
                string.Equals(arg, "--force", StringComparison.OrdinalIgnoreCase)
                || string.Equals(arg, "--force-full", StringComparison.OrdinalIgnoreCase)
                || string.Equals(arg, "--recovery", StringComparison.OrdinalIgnoreCase)))
        {
            return true;
        }

        string? envForce = Environment.GetEnvironmentVariable("SG98_UPDATER_FORCE");
        return string.Equals(envForce, "1", StringComparison.OrdinalIgnoreCase)
            || string.Equals(envForce, "true", StringComparison.OrdinalIgnoreCase)
            || string.Equals(envForce, "yes", StringComparison.OrdinalIgnoreCase);
    }

    private static bool ListPortsRequested(string[] args)
    {
        return args.Any(arg =>
            string.Equals(arg, "--list", StringComparison.OrdinalIgnoreCase)
            || string.Equals(arg, "--list-ports", StringComparison.OrdinalIgnoreCase)
            || string.Equals(arg, "--ports", StringComparison.OrdinalIgnoreCase));
    }

    private static void PrintPortList(string workDir)
    {
        FlashManifest manifest = LoadFlashManifest();
        string[] ports = GetInteractivePorts();
        Console.WriteLine($"Bundle target: {manifest.Role ?? "unknown"} {manifest.EnvName}".TrimEnd());
        Console.WriteLine();

        if (ports.Length == 0)
        {
            Console.WriteLine($"No {PortLabelPlural()} found. Connect the board over USB-C and try again.");
            return;
        }

        Console.WriteLine($"Available {PortLabelPlural()}:");
        for (int i = 0; i < ports.Length; i++)
        {
            string port = ports[i];
            bool probe = IsLikelyUsbFlashingPort(port);
            DeviceIdentity? identity = probe ? TryReadDeviceIdentity(port) : null;
            EspToolChipIdentity? chip = probe && identity is null ? TryReadEspToolChipIdentity(port, workDir) : null;

            Console.WriteLine($"  {i + 1}. {port}{PortHintSuffix(port)}");
            if (identity is not null)
            {
                Console.WriteLine($"     identity: {identity.Role} v{identity.FirmwareVersion} {identity.BuildEnv}".TrimEnd());
            }
            else if (chip is not null)
            {
                string mac = string.IsNullOrWhiteSpace(chip.MacAddress) ? string.Empty : $" {chip.MacAddress}";
                Console.WriteLine($"     hardware: {chip.ChipName}{mac}".TrimEnd());
                Console.WriteLine("     identity: unavailable; SG98 firmware metadata was not readable");
            }
            else
            {
                Console.WriteLine(probe
                    ? "     identity: unavailable"
                    : "     identity: skipped; not a likely USB flashing port");
            }
            Console.WriteLine(probe
                ? $"     force: {DisplayCommandName()} --force --port {QuoteArgumentForDisplay(port)}"
                : "     force: not recommended for this port");
        }

        Console.WriteLine();
        Console.WriteLine($"You can also run {DisplayCommandName()} --force and choose from the port list interactively.");
    }

    private static string DisplayCommandName()
    {
        string? processPath = Environment.ProcessPath;
        if (!string.IsNullOrWhiteSpace(processPath))
        {
            return Path.GetFileName(processPath);
        }

        return OperatingSystem.IsWindows() ? "SG98-Updater.exe" : "./SG98-Updater";
    }

    private static string QuoteArgumentForDisplay(string value)
    {
        if (!value.Any(char.IsWhiteSpace))
        {
            return value;
        }

        return QuoteArgument(value);
    }

    private static void PrintUsage()
    {
        Console.WriteLine("Usage:");
        Console.WriteLine("  SG98-Updater [--port COMx] [--full | --app-only | --force]");
        Console.WriteLine("  SG98-Updater --list");
        Console.WriteLine();
        Console.WriteLine("Modes:");
        Console.WriteLine("  auto        Probe USB-C identity and choose app-only when safe. Default.");
        Console.WriteLine("  --full      Flash the full image set after the normal identity probe.");
        Console.WriteLine("  --app-only  Flash only the application partition.");
        Console.WriteLine("  --force     Skip the USB-C identity probe and full-flash this bundle.");
        Console.WriteLine("  --list      List ports, SG98/ESP32 hints, and force-update commands.");
        Console.WriteLine();
        Console.WriteLine("Use --force only when the connected SG98 target is known but its running");
        Console.WriteLine("firmware cannot report reliable USB-C identity metadata.");
    }

    private static UpdateMode ResolveUpdateMode(string[] args)
    {
        for (int i = 0; i < args.Length; i++)
        {
            if (string.Equals(args[i], "--full", StringComparison.OrdinalIgnoreCase))
            {
                return UpdateMode.Full;
            }
            if (string.Equals(args[i], "--app-only", StringComparison.OrdinalIgnoreCase)
                || string.Equals(args[i], "--application-only", StringComparison.OrdinalIgnoreCase))
            {
                return UpdateMode.Application;
            }
            if (string.Equals(args[i], "--mode", StringComparison.OrdinalIgnoreCase) && i + 1 < args.Length)
            {
                string mode = args[i + 1];
                if (string.Equals(mode, "full", StringComparison.OrdinalIgnoreCase))
                {
                    return UpdateMode.Full;
                }
                if (string.Equals(mode, "application", StringComparison.OrdinalIgnoreCase)
                    || string.Equals(mode, "app", StringComparison.OrdinalIgnoreCase))
                {
                    return UpdateMode.Application;
                }
            }
        }

        return UpdateMode.Auto;
    }

    private static IReadOnlyList<ExtractedFlashSegment> SelectFlashSegments(
        FlashManifest manifest,
        IReadOnlyList<ExtractedFlashSegment> allSegments,
        DeviceIdentity? device,
        UpdateMode requestedMode)
    {
        ExtractedFlashSegment? application = allSegments.FirstOrDefault(segment =>
            string.Equals(segment.Kind, "application", StringComparison.OrdinalIgnoreCase)
            || string.Equals(segment.Offset, manifest.ApplicationOffset ?? "0x10000", StringComparison.OrdinalIgnoreCase));

        if (requestedMode == UpdateMode.Full || application is null)
        {
            return allSegments;
        }

        if (requestedMode == UpdateMode.Application)
        {
            return [application];
        }

        if (string.Equals(manifest.UpdatePolicy?.DefaultMode, "application", StringComparison.OrdinalIgnoreCase)
            && DeviceAllowsApplicationOnly(manifest, device))
        {
            return [application];
        }

        return allSegments;
    }

    private static bool DeviceAllowsApplicationOnly(FlashManifest manifest, DeviceIdentity? device)
    {
        if (device is null || !device.SupportsAppOnlyUpdate)
        {
            return false;
        }

        if (!DeviceMeetsAppOnlySince(manifest, device))
        {
            return false;
        }

        if (!string.IsNullOrWhiteSpace(manifest.Role)
            && !string.Equals(manifest.Role, device.Role, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        if (!string.IsNullOrWhiteSpace(manifest.EnvName)
            && !string.IsNullOrWhiteSpace(device.BuildEnv)
            && !string.Equals(manifest.EnvName, device.BuildEnv, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        if (manifest.Application is not null)
        {
            if (!string.IsNullOrWhiteSpace(manifest.Application.Offset)
                && !string.IsNullOrWhiteSpace(device.ApplicationOffset)
                && !string.Equals(manifest.Application.Offset, device.ApplicationOffset, StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }

            if (manifest.Application.Size > 0
                && device.ApplicationSize > 0
                && manifest.Application.Size > device.ApplicationSize)
            {
                return false;
            }
        }

        return true;
    }

    private static bool DeviceMeetsAppOnlySince(FlashManifest manifest, DeviceIdentity device)
    {
        string? appOnlySince = manifest.UpdatePolicy?.AppOnlySince;
        if (string.IsNullOrWhiteSpace(appOnlySince))
        {
            return true;
        }

        if (string.IsNullOrWhiteSpace(device.FirmwareVersion))
        {
            return false;
        }

        return CompareVersionLabels(device.FirmwareVersion, appOnlySince) >= 0;
    }

    private static int CompareVersionLabels(string left, string right)
    {
        string[] leftParts = VersionParts(left);
        string[] rightParts = VersionParts(right);
        int count = Math.Max(leftParts.Length, rightParts.Length);
        for (int i = 0; i < count; i++)
        {
            string leftPart = i < leftParts.Length ? leftParts[i] : "0";
            string rightPart = i < rightParts.Length ? rightParts[i] : "0";

            bool leftIsNumber = int.TryParse(leftPart, out int leftNumber);
            bool rightIsNumber = int.TryParse(rightPart, out int rightNumber);
            int comparison = leftIsNumber && rightIsNumber
                ? leftNumber.CompareTo(rightNumber)
                : string.Compare(leftPart, rightPart, StringComparison.OrdinalIgnoreCase);
            if (comparison != 0)
            {
                return comparison;
            }
        }

        return 0;
    }

    private static string[] VersionParts(string value)
    {
        string cleaned = value.Trim();
        if (cleaned.StartsWith("v", StringComparison.OrdinalIgnoreCase))
        {
            cleaned = cleaned[1..];
        }
        int suffixIndex = cleaned.IndexOf('-');
        if (suffixIndex >= 0)
        {
            cleaned = cleaned[..suffixIndex];
        }
        return cleaned.Split(['.', '_'], StringSplitOptions.RemoveEmptyEntries);
    }

    private static string UpdateModeDescription(
        IReadOnlyList<ExtractedFlashSegment> selectedSegments,
        IReadOnlyList<ExtractedFlashSegment> allSegments)
    {
        return selectedSegments.Count == allSegments.Count
            ? "full flash"
            : "application only";
    }

    private static DeviceIdentity? TryReadDeviceIdentity(string portName)
    {
        try
        {
            using SerialPort port = new(portName, 115200)
            {
                DtrEnable = false,
                RtsEnable = false,
                NewLine = "\n",
                ReadTimeout = 250,
                WriteTimeout = 250,
            };

            port.Open();
            port.DiscardInBuffer();
            port.WriteLine("api device");

            Stopwatch deadline = Stopwatch.StartNew();
            bool retried = false;
            while (deadline.Elapsed < TimeSpan.FromMilliseconds(1800))
            {
                try
                {
                    string line = port.ReadLine().Trim();
                    DeviceIdentity? identity = TryParseDeviceIdentity(line);
                    if (identity is not null)
                    {
                        return identity;
                    }
                }
                catch (TimeoutException)
                {
                    if (!retried && deadline.Elapsed > TimeSpan.FromMilliseconds(600))
                    {
                        port.WriteLine("api device");
                        retried = true;
                    }
                }
            }
        }
        catch
        {
            return null;
        }
        finally
        {
            System.Threading.Thread.Sleep(250);
        }

        return null;
    }

    private static EspToolChipIdentity? TryReadEspToolChipIdentity(string portName, string workDir)
    {
        try
        {
            var flasher = ResolveFlasher(workDir);
            string args = flasher.ArgumentsPrefix
                + $"--chip esp32s3 --port {QuoteArgument(portName)} --baud 115200 "
                + "--connect-attempts 1 --before default_reset --after hard_reset chip_id";
            ProcessCaptureResult result = RunProcessCapture(flasher.FileName, args, 7000);
            if (result.ExitCode != 0)
            {
                return null;
            }

            return ParseEspToolChipIdentity(result.Output);
        }
        catch
        {
            return null;
        }
    }

    private static EspToolChipIdentity? ParseEspToolChipIdentity(string output)
    {
        if (string.IsNullOrWhiteSpace(output))
        {
            return null;
        }

        string chipName = string.Empty;
        string mac = string.Empty;
        foreach (string rawLine in output.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries))
        {
            string line = rawLine.Trim();
            int chipIndex = line.IndexOf("Chip is ", StringComparison.OrdinalIgnoreCase);
            if (chipIndex >= 0)
            {
                chipName = line[(chipIndex + "Chip is ".Length)..].Trim();
                int detailIndex = chipName.IndexOf(" (", StringComparison.Ordinal);
                if (detailIndex > 0)
                {
                    chipName = chipName[..detailIndex].Trim();
                }
            }

            int detectIndex = line.IndexOf("Detecting chip type...", StringComparison.OrdinalIgnoreCase);
            if (string.IsNullOrWhiteSpace(chipName) && detectIndex >= 0)
            {
                string detected = line[(detectIndex + "Detecting chip type...".Length)..].Trim();
                if (detected.StartsWith("ESP", StringComparison.OrdinalIgnoreCase))
                {
                    chipName = detected;
                }
            }

            if (line.StartsWith("MAC:", StringComparison.OrdinalIgnoreCase))
            {
                mac = line["MAC:".Length..].Trim();
            }
        }

        if (string.IsNullOrWhiteSpace(chipName) && output.Contains("ESP32-S3", StringComparison.OrdinalIgnoreCase))
        {
            chipName = "ESP32-S3";
        }

        if (string.IsNullOrWhiteSpace(chipName))
        {
            return null;
        }

        return new EspToolChipIdentity(chipName, mac);
    }

    private static DeviceIdentity? TryParseDeviceIdentity(string line)
    {
        if (string.IsNullOrWhiteSpace(line) || !line.TrimStart().StartsWith('{'))
        {
            return null;
        }

        try
        {
            using JsonDocument document = JsonDocument.Parse(line);
            JsonElement root = document.RootElement;
            if (!string.Equals(JsonString(root, "type"), "device", StringComparison.OrdinalIgnoreCase))
            {
                return null;
            }

            return new DeviceIdentity(
                JsonString(root, "role") ?? string.Empty,
                JsonString(root, "firmwareVersion") ?? string.Empty,
                JsonString(root, "buildEnv") ?? string.Empty,
                JsonBool(root, "supportsAppOnlyUpdate"),
                JsonString(root, "applicationOffset") ?? string.Empty,
                JsonLong(root, "applicationSize"));
        }
        catch (JsonException)
        {
            return null;
        }
    }

    private static string? JsonString(JsonElement root, string name)
    {
        return root.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String
            ? value.GetString()
            : null;
    }

    private static bool JsonBool(JsonElement root, string name)
    {
        return root.TryGetProperty(name, out JsonElement value)
            && value.ValueKind == JsonValueKind.True;
    }

    private static long JsonLong(JsonElement root, string name)
    {
        return root.TryGetProperty(name, out JsonElement value)
            && value.ValueKind == JsonValueKind.Number
            && value.TryGetInt64(out long result)
                ? result
                : 0;
    }

    private static string BuildWriteFlashArgs(string port, IReadOnlyList<ExtractedFlashSegment> flashSegments)
    {
        StringBuilder builder = new();
        builder.Append($"--chip esp32s3 --port {QuoteArgument(port)} --baud {DefaultBaud} ");
        builder.Append("--before default_reset --after hard_reset ");
        builder.Append("write_flash -z --flash_size detect");

        foreach (ExtractedFlashSegment segment in flashSegments)
        {
            builder.Append(' ');
            builder.Append(segment.Offset);
            builder.Append(' ');
            builder.Append(QuoteArgument(segment.Path));
        }

        return builder.ToString();
    }

    private static long ParseOffset(FlashSegment segment)
    {
        string value = segment.Offset.Trim();
        if (value.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
        {
            value = value[2..];
        }

        return Convert.ToInt64(value, 16);
    }

    private static string QuoteArgument(string value) => $"\"{value}\"";

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
            if (IsUsuallyWrongPort(explicitPort!))
            {
                throw new InvalidOperationException(
                    $"{explicitPort} is not an ESP32-S3 USB flashing port. Connect the SG98 board over USB-C and use --list to find a /dev/cu.usbmodem* or /dev/cu.usbserial* port.");
            }
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

        if (ports.Length == 1 && IsSelectableInteractivePort(ports[0]))
        {
            return ports[0];
        }

        Console.WriteLine($"Available {PortLabelPlural()}:");
        for (int i = 0; i < ports.Length; i++)
        {
            Console.WriteLine($"  {i + 1}. {ports[i]}{PortHintSuffix(ports[i])}");
        }
        Console.WriteLine();

        string[] selectablePorts = ports
            .Where(IsSelectableInteractivePort)
            .ToArray();
        if (selectablePorts.Length == 0)
        {
            throw new InvalidOperationException(
                $"No SG98 USB flashing port is visible. Connect the board over USB-C and look for a /dev/cu.usbmodem* or /dev/cu.usbserial* port.");
        }

        if (likelyPorts.Length > 0)
        {
            Console.WriteLine("Ports marked [USB] are the best SG98 flashing candidates.");
            Console.WriteLine();
        }
        while (true)
        {
            Console.Write($"Choose {PortLabelSingular()} number: ");
            string? input = Console.ReadLine();
            if (!int.TryParse(input, out int selected) || selected < 1 || selected > ports.Length)
            {
                throw new InvalidOperationException($"Invalid {PortLabelSingular()} selection.");
            }

            string selectedPort = ports[selected - 1];
            if (IsSelectableInteractivePort(selectedPort))
            {
                return selectedPort;
            }

            Console.WriteLine($"{selectedPort} is marked [skip] and is not an ESP32-S3 USB flashing port.");
        }
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

    private static ProcessCaptureResult RunProcessCapture(string fileName, string arguments, int timeoutMs)
    {
        using Process process = new();
        process.StartInfo.FileName = fileName;
        process.StartInfo.Arguments = arguments;
        process.StartInfo.UseShellExecute = false;
        process.StartInfo.RedirectStandardOutput = true;
        process.StartInfo.RedirectStandardError = true;
        process.StartInfo.CreateNoWindow = true;

        process.Start();
        Task<string> stdout = process.StandardOutput.ReadToEndAsync();
        Task<string> stderr = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(timeoutMs))
        {
            try
            {
                process.Kill(entireProcessTree: true);
            }
            catch
            {
                // Best-effort cleanup for a diagnostic probe.
            }

            return new ProcessCaptureResult(-1, string.Empty);
        }

        string output = stdout.GetAwaiter().GetResult() + "\n" + stderr.GetAwaiter().GetResult();
        return new ProcessCaptureResult(process.ExitCode, output);
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

    private static bool IsSelectableInteractivePort(string port) => !IsUsuallyWrongPort(port);

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
        }
        else
        {
            if (!string.IsNullOrWhiteSpace(selectedPort) && !IsLikelyUsbFlashingPort(selectedPort))
            {
                Console.WriteLine($"The selected port {selectedPort} does not look like the ESP32-S3 USB flashing port.");
            }

            Console.WriteLine("Prefer a /dev/cu.usbmodem* or /dev/cu.usbserial* device for the SG98 board.");
            Console.WriteLine("If flashing fails because esptool is missing, install it with:");
            Console.WriteLine("python3 -m pip install --user esptool");
        }

        Console.WriteLine();
        Console.WriteLine("If this is the original SG98 controller board and the upgrade failed:");
        Console.WriteLine($"1. Run {DisplayCommandName()} --list.");
        Console.WriteLine("2. Pick the port that shows SG98 identity or ESP32-S3 hardware.");
        if (!string.IsNullOrWhiteSpace(selectedPort) && IsLikelyUsbFlashingPort(selectedPort))
        {
            Console.WriteLine($"3. Retry with: {DisplayCommandName()} --force --port {QuoteArgumentForDisplay(selectedPort)}");
        }
        else
        {
            Console.WriteLine($"3. Retry with the listed command: {DisplayCommandName()} --force --port <port>");
        }
        Console.WriteLine("4. If no ESP32-S3 appears, hold BOOT/BOOT0, tap EN/RESET, then run --list again.");
        Console.WriteLine();
        Console.WriteLine("Use the controller updater for the controller and the remote updater for the remote.");
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
        public string? Schema { get; init; }
        public string? Version { get; init; }
        public string? GitSha { get; init; }
        public string? Role { get; init; }
        public string EnvName { get; init; } = string.Empty;
        public string? BoardName { get; init; }
        public string? ApplicationOffset { get; init; }
        public ApplicationInfo? Application { get; init; }
        public UpdatePolicy? UpdatePolicy { get; init; }
        public List<FlashSegment> Segments { get; init; } = [];
    }

    private sealed class ApplicationInfo
    {
        public string? Offset { get; init; }
        public string? FileName { get; init; }
        public string? Sha256 { get; init; }
        public long Size { get; init; }
    }

    private sealed class UpdatePolicy
    {
        public string? DefaultMode { get; init; }
        public string? AppOnlySince { get; init; }
        public string? ApplicationOffset { get; init; }
        public string? RequiresDeviceCapability { get; init; }
        public string? FallbackMode { get; init; }
        public string? FallbackReason { get; init; }
    }

    private sealed class FlashSegment
    {
        public string Offset { get; init; } = string.Empty;
        public string FileName { get; init; } = string.Empty;
        public string? SourceName { get; init; }
        public string? Kind { get; init; }
    }

    private sealed record ExtractedFlashSegment(string Offset, string FileName, string Path, string Kind);

    private sealed record ProcessCaptureResult(int ExitCode, string Output);

    private sealed record EspToolChipIdentity(string ChipName, string MacAddress);

    private sealed record DeviceIdentity(
        string Role,
        string FirmwareVersion,
        string BuildEnv,
        bool SupportsAppOnlyUpdate,
        string ApplicationOffset,
        long ApplicationSize);

    private enum UpdateMode
    {
        Auto,
        Application,
        Full,
    }
}
