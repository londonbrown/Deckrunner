// wkprobe tweak <regex> [flat-regex]: records of the lab copy's TweakDB (r6\cache\tweakdb.bin) whose name matches, with
// their flats (all, or only those whose name matches flat-regex). Read-only, through WolvenKit's public TweakDB reader.
// P1-3 (2026-11-16): which GameplayRestriction records exist and which gameplay tags each one puts on V.
using System.Text.RegularExpressions;
using WolvenKit.Common.Services;
using WolvenKit.RED4.Types;

static class Tweak
{
    public static int Run(string[] args, IHashService hashes, string game)
    {
        if (args.Length < 2) { Console.Error.WriteLine("usage: tweak <record regex> [flat regex]"); return 2; }
        var svc = new TweakDBService(hashes);
        svc.LoadDB(Path.Combine(game, @"r6\cache\tweakdb.bin")).GetAwaiter().GetResult();
        var rx = new Regex(args[1], RegexOptions.IgnoreCase);
        var fx = args.Length > 2 ? new Regex(args[2], RegexOptions.IgnoreCase) : null;
        var n = 0;
        foreach (var id in TweakDBService.GetRecords())
        {
            var name = id.TryGetResolvedText(out var s) ? s : null;
            if (name == null || !rx.IsMatch(name)) continue;
            ++n;
            TweakDBService.TryGetType(id, out var type);
            Console.WriteLine($"{name}  [{type?.Name}]");
            var rec = TweakDBService.GetRecord(id);
            if (rec == null) continue;
            foreach (var p in rec.GetType().GetProperties().Where(p => p.GetIndexParameters().Length == 0 && p.DeclaringType != typeof(object)))
            {
                if (fx != null && !fx.IsMatch(p.Name)) continue;
                object v;
                try { v = p.GetValue(rec); } catch { continue; }
                Console.WriteLine($"    {p.Name} = {Show(v)}");
            }
        }
        Console.Error.WriteLine($"{n} records");
        return 0;
    }

    static string Show(object v) => v switch
    {
        null => "null",
        TweakDBID t => t.TryGetResolvedText(out var s) ? s : t.ToString(),
        CName c => $"\"{c.GetResolvedText()}\"",
        System.Collections.IEnumerable e and not string => "[" + string.Join(", ", e.Cast<object>().Select(Show)) + "]",
        _ => v.ToString() ?? "",
    };
}
