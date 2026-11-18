// Write side of the lab tool: JSON -> CR2W, CR2W -> JSON for round-trip checks, and archive packing.
// Everything it writes stays in the lab (never the real install).
using System.Reflection;
using WolvenKit.Common.Conversion;
using WolvenKit.Common.Services;
using WolvenKit.Core.Interfaces;
using WolvenKit.RED4.Archive.CR2W;
using WolvenKit.RED4.Archive.IO;
using WolvenKit.RED4.CR2W.JSON;

static class Writer
{
    public static int Run(string[] args, HashService hashes)
    {
        if (args[0] == "cr2w" && args.Length >= 3)
        {
            var dto = RedJsonSerializer.Deserialize<RedFileDto>(File.ReadAllText(args[1]), new RedJsonSerializerOptions());
            if (dto?.Data == null) { Console.Error.WriteLine("JSON did not deserialize to a CR2W file"); return 1; }
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(args[2]))!);
            using (var fs = File.Create(args[2]))
            using (var w = new CR2WWriter(fs)) w.WriteFile(dto.Data);
            Console.Error.WriteLine($"{args[1]} -> {args[2]} ({new FileInfo(args[2]).Length} bytes)");
            return 0;
        }
        if (args[0] == "cr2wjson" && args.Length >= 3)
        {
            using var fs = File.OpenRead(args[1]);
            using var r = new CR2WReader(fs);
            if (r.ReadFile(out CR2WFile cr2w, true) != EFileReadErrorCodes.NoError || cr2w == null) { Console.Error.WriteLine("not a readable CR2W file"); return 1; }
            File.WriteAllText(args[2], RedJsonSerializer.Serialize(new RedFileDto(cr2w, false), new RedJsonSerializerOptions()));
            Console.Error.WriteLine($"{args[1]} -> {args[2]}");
            return 0;
        }
        if (args[0] == "pack" && args.Length >= 3)
        {
            var logger = DispatchProxy.Create<ILoggerService, QuietLog>();
            var aw = new ArchiveWriter(hashes, logger);
            bool ok;
            using (var fs = File.Create(args[2])) ok = aw.WriteArchive(new DirectoryInfo(args[1]), fs);
            Console.Error.WriteLine($"{args[1]} -> {args[2]}: {(ok ? "ok" : "FAILED")}");
            if (!ok) return 1;
            // Read the archive back: every file must be there under its depot path's hash, and decode to the same content
            // as the file that went in. The archive stores a file's buffers as separate, compressed segments, so the
            // bytes differ for files with buffers (meshes, textures, entities); the decoded JSON must not.
            if (new ArchiveReader().ReadArchive(args[2], hashes, out var ar) != EFileReadErrorCodes.NoError || ar == null) { Console.Error.WriteLine("the archive does not read back"); return 1; }
            var root = Path.GetFullPath(args[1]);
            var bad = 0;
            static string Json(Stream s) { using var r = new CR2WReader(s); return r.ReadFile(out CR2WFile c, true) == EFileReadErrorCodes.NoError && c != null ? RedJsonSerializer.Serialize(new RedFileDto(c, false), new RedJsonSerializerOptions()) : null; }
            static string Body(string json) => json == null ? null : json[(json.IndexOf("\"Data\"", StringComparison.Ordinal))..];  // the header holds a timestamp
            foreach (var f in Directory.GetFiles(root, "*", SearchOption.AllDirectories))
            {
                var depot = Path.GetRelativePath(root, f);
                var hash = WolvenKit.Common.FNV1A.FNV1A64HashAlgorithm.HashString(depot);
                if (!ar.Files.ContainsKey(hash)) { Console.Error.WriteLine($"  {depot}: missing (0x{hash:x16})"); ++bad; continue; }
                using var ms = new MemoryStream();
                ar.CopyFileToStream(ms, hash, false);
                ms.Position = 0;
                string a, b;
                using (var src = File.OpenRead(f)) a = Body(Json(src));
                b = Body(Json(ms));
                var same = a != null && a == b;
                Console.Error.WriteLine($"  {depot} (0x{hash:x16}): {(same ? "same content" : a == null ? "source unreadable" : b == null ? "UNREADABLE from the archive" : "DIFFERENT content")}");
                if (!same) ++bad;
            }
            return bad == 0 ? 0 : 1;
        }
        Console.Error.WriteLine("usage: cr2w <in.json> <out> | cr2wjson <in> <out.json> | pack <folder> <out.archive>");
        return 2;
    }
}

public class QuietLog : DispatchProxy
{
    protected override object Invoke(MethodInfo targetMethod, object[] args)
    {
        if (targetMethod.Name is "Error" or "Warning" && args?.Length > 0) Console.Error.WriteLine($"wolvenkit {targetMethod.Name}: {args[^1]}");
        return targetMethod.ReturnType.IsValueType && targetMethod.ReturnType != typeof(void) ? Activator.CreateInstance(targetMethod.ReturnType) : null;
    }
}

// Stand-in for WolvenKit services the importers take but these commands do not need: void and null for everything,
// true for Boolean results, completed tasks.
public class Stub : DispatchProxy
{
    protected override object Invoke(MethodInfo targetMethod, object[] args)
    {
        var t = targetMethod.ReturnType;
        if (t == typeof(void)) return null;
        if (t == typeof(bool)) return true;
        if (t == typeof(Task)) return Task.CompletedTask;
        if (t.IsGenericType && t.GetGenericTypeDefinition() == typeof(Task<>))
        {
            var r = t.GetGenericArguments()[0];
            var v = r == typeof(bool) ? true : r.IsValueType ? System.Activator.CreateInstance(r) : null;
            return typeof(Task).GetMethod(nameof(Task.FromResult))!.MakeGenericMethod(r).Invoke(null, new[] { v });
        }
        return t.IsValueType ? System.Activator.CreateInstance(t) : null;
    }
}
