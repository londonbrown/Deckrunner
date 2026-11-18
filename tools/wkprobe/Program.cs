// Deckrunner lab tool (Stage C3): reads the game's archives with WolvenKit's libraries, read-only.
//   wkprobe list <regex>                 archive files whose path matches (case-insensitive)
//   wkprobe json <path> <out.json>       extract one file and write it as WolvenKit JSON
//   wkprobe types <regex>                WolvenKit API discovery (type names)
//   wkprobe --members <type>...          WolvenKit API discovery (members)
//   wkprobe raw <path> <out.bin>         extract one file's bytes unchanged
//   wkprobe cr2w <in.json> <out>         WolvenKit JSON -> CR2W file (local, no archives read)
//   wkprobe cr2wjson <in> <out.json>     local CR2W file -> WolvenKit JSON (round-trip checks)
//   wkprobe pack <folder> <out.archive>  pack a folder laid out by depot path into one .archive
//   wkprobe patch-skate <in> <out>       Stage C3: splice the Skate pose override into player_base.animgraph
// Game data is read from the game folder in DKR_GAME and decompressed with the game's own oo2ext; WolvenKit's libraries
// are loaded from DKR_WOLVENKIT (Deckrunner's setup downloads a pinned WolvenKit release there). Output is the user's own
// CD Projekt Red data: it stays on their machine and must never be committed or shared.
//
// Copyright (C) 2026 Deckrunner contributors. This program links WolvenKit (GPL-3.0) and is free software: you can
// redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later version. It is distributed WITHOUT ANY
// WARRANTY; see LICENSE (GPL-3.0) beside this file.
using System.Runtime.Loader;
using System.Text.RegularExpressions;
using WolvenKit.Common.Services;
using WolvenKit.Core.Compression;
using WolvenKit.RED4.Archive;
using WolvenKit.RED4.Archive.CR2W;
using WolvenKit.RED4.Archive.IO;
using WolvenKit.RED4.CR2W.JSON;

if (Env.Wk == null || Env.Game == null)
{
    Console.Error.WriteLine("set DKR_WOLVENKIT (a WolvenKit folder) and DKR_GAME (the Cyberpunk 2077 folder)");
    return 2;
}
AssemblyLoadContext.Default.Resolving += (ctx, name) => { var p = Path.Combine(Env.Wk, name.Name + ".dll"); return File.Exists(p) ? ctx.LoadFromAssemblyPath(p) : null; };
if (args.Length == 0) { Console.WriteLine("usage: list <regex> | json <path> <out.json> | types <regex> | --members <type>..."); return 2; }
return Run(args);

static int Run(string[] args)
{
    if (args[0] == "--members" || args[0] == "types")
    {
        foreach (var d in new[] { "WolvenKit.RED4", "WolvenKit.Common", "WolvenKit.Core", "WolvenKit.Modkit" })
            AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(Env.Wk, d + ".dll"));
        if (args[0] == "--members") { foreach (var n in args.Skip(1)) Members.Dump(n); return 0; }
        foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
        {
            Type[] types;
            try { types = asm.GetTypes(); } catch (System.Reflection.ReflectionTypeLoadException e) { types = e.Types.Where(t => t != null).ToArray(); }
            foreach (var t in types.Where(t => (t.IsPublic || t.IsNestedPublic) && Regex.IsMatch(t.FullName ?? "", args[1])).OrderBy(t => t.FullName)) Console.WriteLine(t.FullName);
        }
        return 0;
    }
    if (!Oodle.Load(Path.Combine(Env.Game, @"bin\x64\oo2ext_7_win64.dll"))) Console.Error.WriteLine("warning: oo2ext not loaded");
    var hashes = new HashService();
    hashes.Load();
    if (args[0] is "cr2w" or "cr2wjson" or "pack") return Writer.Run(args, hashes);
    if (args[0] == "patch-skate") return Patch.Run(args);
    if (args[0] == "tweak") return Tweak.Run(args, hashes, Env.Game);
    var archives = new List<Archive>();
    var reader = new ArchiveReader();
    foreach (var dir in new[] { @"archive\pc\content", @"archive\pc\ep1" })
    {
        var full = Path.Combine(Env.Game, dir);
        if (!Directory.Exists(full)) continue;
        foreach (var f in Directory.GetFiles(full, "*.archive"))
            if (reader.ReadArchive(f, hashes, out var ar) == WolvenKit.RED4.Archive.IO.EFileReadErrorCodes.NoError && ar != null) archives.Add(ar);
    }
    Console.Error.WriteLine($"{archives.Count} archives");
    if (args[0] is "ents" or "board-build") return Board.Run(args, archives, hashes);
    if (args[0] == "list")
    {
        var rx = new Regex(args[1], RegexOptions.IgnoreCase);
        var n = 0;
        foreach (var ar in archives)
            foreach (var kv in ar.Files)
            {
                var name = hashes.Get(kv.Key) ?? "";
                if (rx.IsMatch(name)) { Console.WriteLine($"{name}\t{ar.Name}\t0x{kv.Key:x16}"); ++n; }
            }
        Console.Error.WriteLine($"{n} files");
        return 0;
    }
    if ((args[0] == "json" || args[0] == "raw") && args.Length >= 3)
    {
        foreach (var ar in archives)
            foreach (var kv in ar.Files)
            {
                if (!string.Equals(hashes.Get(kv.Key), args[1], StringComparison.OrdinalIgnoreCase)) continue;
                // The same path can sit in several archives (a later one patches an earlier one); try each copy.
                try
                {
                    using var ms = new MemoryStream();
                    ar.CopyFileToStream(ms, kv.Key, Environment.GetEnvironmentVariable("WK_SEGDECOMP") != null);
                    ms.Position = 0;
                    if (args[0] == "raw") { File.WriteAllBytes(args[2], ms.ToArray()); Console.Error.WriteLine($"{args[1]} ({ar.Name}, {ms.Length} bytes) -> {args[2]}"); return 0; }
                    using var r = new CR2WReader(ms);
                    if (r.ReadFile(out CR2WFile cr2w, Environment.GetEnvironmentVariable("WK_NOBUF") == null) != WolvenKit.RED4.Archive.IO.EFileReadErrorCodes.NoError || cr2w == null) { Console.Error.WriteLine($"{ar.Name}: not a readable CR2W file"); continue; }
                    File.WriteAllText(args[2], RedJsonSerializer.Serialize(new WolvenKit.Common.Conversion.RedFileDto(cr2w, false), new RedJsonSerializerOptions()));
                    Console.Error.WriteLine($"{args[1]} ({ar.Name}, {ms.Length} bytes) -> {args[2]}");
                    return 0;
                }
                catch (Exception e) { Console.Error.WriteLine($"{ar.Name}: {e}"); }
            }
        Console.Error.WriteLine("not found");
        return 1;
    }
    Console.Error.WriteLine("unknown command");
    return 2;
}

static class Env
{
    public static readonly string Wk = Environment.GetEnvironmentVariable("DKR_WOLVENKIT");
    public static readonly string Game = Environment.GetEnvironmentVariable("DKR_GAME");
}
