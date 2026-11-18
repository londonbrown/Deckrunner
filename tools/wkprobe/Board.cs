// Stage C3b (the skateboard): lab-only helpers that read the game's archives.
//   wkprobe ents <regex> <max> [simple]   summarise entity templates (simple: one entity and one static mesh only)
//   wkprobe board-build <raw> <out>        build the board's game files from tools/board/board_glb.py's output
using System.Reflection;
using System.Text.RegularExpressions;
using WolvenKit.Common.Interfaces;
using WolvenKit.Common.Model;
using WolvenKit.Common.Model.Arguments;
using WolvenKit.Core.Interfaces;
using WolvenKit.Modkit.RED4;
using WolvenKit.RED4.CR2W;
using WolvenKit.Common.Services;
using WolvenKit.RED4.Archive;
using WolvenKit.RED4.Archive.Buffer;
using WolvenKit.RED4.Archive.CR2W;
using WolvenKit.RED4.Archive.IO;
using WolvenKit.RED4.Types;

static class Board
{
    public static int Run(string[] args, List<Archive> archives, HashService hashes)
    {
        if (args[0] == "ents" && args.Length >= 3)
        {
            var rx = new Regex(args[1], RegexOptions.IgnoreCase);
            var max = int.Parse(args[2]);
            var shown = 0;
            foreach (var ar in archives)
                foreach (var kv in ar.Files)
                {
                    var name = hashes.Get(kv.Key) ?? "";
                    if (!name.EndsWith(".ent", StringComparison.OrdinalIgnoreCase) || !rx.IsMatch(name)) continue;
                    try
                    {
                        var cr2w = Read(ar, kv.Key, out var size);
                        if (cr2w?.RootChunk is not entEntityTemplate t) continue;
                        // "simple": only templates whose compiled data is one entity and one static mesh component.
                        if (args.Length > 3 && args[3] == "simple")
                        {
                            var chunks = (t.CompiledData?.Data as RedPackage)?.Chunks;
                            if (chunks == null || chunks.OfType<entIComponent>().Count() != 1 || !chunks.OfType<entMeshComponent>().Any() ||
                                chunks.OfType<entMeshComponent>().First().GetType() != typeof(entMeshComponent) || t.Appearances.Count > 1) continue;
                        }
                        Console.WriteLine($"{name}  ({size} bytes, {ar.Name})");
                        Describe(t);
                        if (++shown >= max) return 0;
                    }
                    catch (Exception e) { Console.WriteLine($"{name}: {e.GetType().Name}: {e.Message}"); }
                }
            return 0;
        }
        if (args[0] == "board-build" && args.Length >= 3) return BoardBuild(new DirectoryInfo(args[1]), new DirectoryInfo(args[2]), archives, hashes);
        Console.Error.WriteLine("usage: ents <regex> <max> [simple] | board-build <raw> <out>");
        return 2;
    }

    const string Dir = @"deckrunner\board";
    const string TemplateEnt = @"base\quest\side_quests\sq024\entities\sq024_beer_bottle.ent";  // gameObject + one entMeshComponent
    const string TemplateMesh = @"base\environment\decoration\electronics\hardware\credit_chip\credit_chip_a.mesh";  // one material, static
    const string TemplateMi = @"base\environment\decoration\electronics\hardware\textures\credit_chip_01_noname.mi";  // metal_base.remt
    static readonly string[] Parts = { "deck", "truck", "wheel" };

    // WolvenKit's public import entry point (what its app and CLI use): it picks the importer from the raw file's
    // extension and takes that importer's settings from the registered arguments.
    static bool Import(IModTools tools, DirectoryInfo raw, string rel, DirectoryInfo outDir, ImportArgs a) =>
        tools.Import(new RedRelativePath(raw, rel), new GlobalImportArgs().Register(a), outDir, false).GetAwaiter().GetResult();

    // Stage C3b: Skate's board as the game's own kinds of files, built on three small vanilla templates.
    //   <out>\deckrunner\board\deckrunner_board.ent   gameObject + entMeshComponent "deckrunner_board" (from TemplateEnt)
    //   <out>\deckrunner\board\deckrunner_board.mesh  the glb's geometry imported by WolvenKit into TemplateMesh, one
    //                                                  material entry per part
    //   <out>\deckrunner\board\board_<part>.mi         metal_base with the part's colour, normal, roughness, metalness
    //   <out>\deckrunner\board\textures\*.xbm          from the glb's PNGs
    static int BoardBuild(DirectoryInfo raw, DirectoryInfo outDir, List<Archive> archives, HashService hashes)
    {
        outDir.Create();
        var log = DispatchProxy.Create<ILoggerService, QuietLog>();
        var hook = DispatchProxy.Create<IHookService, Stub>();
        var tools = new ModTools(log, DispatchProxy.Create<WolvenKit.Core.Services.IProgressService<double>, Stub>(), hashes,
                                 new Red4ParserService(hashes, log, hook), DispatchProxy.Create<WolvenKit.Common.IArchiveManager, Stub>(), hook);
        string OutPath(string depot) { var p = Path.Combine(outDir.FullName, depot); Directory.CreateDirectory(Path.GetDirectoryName(p)!); return p; }
        byte[] Vanilla(string depot)
        {
            var h = WolvenKit.Common.FNV1A.FNV1A64HashAlgorithm.HashString(depot);
            foreach (var ar in archives)
                if (ar.Files.ContainsKey(h)) { using var ms = new MemoryStream(); ar.CopyFileToStream(ms, h, false); return ms.ToArray(); }
            throw new FileNotFoundException(depot);
        }
        CR2WFile Parse(byte[] bytes) { using var r = new CR2WReader(new MemoryStream(bytes)); return r.ReadFile(out CR2WFile f, true) == EFileReadErrorCodes.NoError ? f : throw new InvalidDataException("unreadable CR2W"); }
        void Write(CR2WFile f, string depot) { using var fs = File.Create(OutPath(depot)); using var w = new CR2WWriter(fs); w.WriteFile(f); }
        var ok = true;

        // 1. Textures.
        foreach (var part in Parts)
            foreach (var kind in new[] { "d", "n", "r", "m" })
            {
                var rel = $@"{Dir}\textures\board_{part}_{kind}.png";
                var a = new XbmImportArgs
                {
                    TextureGroup = kind == "n" ? Enums.GpuWrapApieTextureGroup.TEXG_Generic_Normal : kind == "d" ? Enums.GpuWrapApieTextureGroup.TEXG_Generic_Color : Enums.GpuWrapApieTextureGroup.TEXG_Generic_Grayscale,
                    IsGamma = kind == "d",
                    Compression = kind == "n" ? Enums.ETextureCompression.TCM_Normalmap : kind == "d" ? Enums.ETextureCompression.TCM_QualityColor : Enums.ETextureCompression.TCM_QualityR,
                    RawFormat = kind == "d" || kind == "n" ? Enums.ETextureRawFormat.TRF_TrueColor : Enums.ETextureRawFormat.TRF_Grayscale,
                    GenerateMipMaps = true,
                    IsStreamable = true,
                };
                var done = Import(tools, raw, rel, outDir, a);
                Console.Error.WriteLine($"  texture {rel}: {(done ? "imported" : "FAILED")}");
                ok &= done;
            }

        // 2. Mesh: the template first, then WolvenKit's glTF import into it (it looks for the .mesh next to the .glb's path).
        var meshDepot = $@"{Dir}\deckrunner_board.mesh";
        File.WriteAllBytes(OutPath(meshDepot), Vanilla(TemplateMesh));
        var g = new GltfImportArgs { ImportFormat = GltfImportAsFormat.Mesh, ImportMaterials = false, ImportGarmentSupport = false, OverrideMeshNameWithNodeName = false };
        var imported = Import(tools, raw, $@"{Dir}\deckrunner_board.glb", outDir, g);
        Console.Error.WriteLine($"  mesh import: {(imported ? "ok" : "FAILED")}");
        ok &= imported;
        var mesh = Parse(File.ReadAllBytes(OutPath(meshDepot)));
        if (mesh.RootChunk is not CMesh cm) throw new InvalidDataException("the imported mesh is not a CMesh");
        // One local material instance per part, each on that part's .mi; the appearance names a material per chunk.
        cm.MaterialEntries.Clear();
        cm.PreloadLocalMaterialInstances.Clear();
        for (var i = 0; i < Parts.Length; ++i)
        {
            cm.MaterialEntries.Add(new CMeshMaterialEntry { Name = $"board_{Parts[i]}", Index = (ushort)i, IsLocalInstance = true });
            cm.PreloadLocalMaterialInstances.Add(new CHandle<IMaterial>(new CMaterialInstance
            {
                BaseMaterial = new CResourceReference<IMaterial>($@"{Dir}\board_{Parts[i]}.mi"),
                ResourceVersion = 4,
                Values = new CArray<CKeyValuePair>(),
            }));
        }
        // The import keeps the template's appearance (one chunk); the render blob holds one chunk per glTF node, in order.
        var chunkCount = cm.RenderResourceBlob?.Chunk is rendRenderMeshBlob blob ? blob.Header.RenderChunkInfos.Count : 0;
        foreach (var app in cm.Appearances)
        {
            app.Chunk.ChunkMaterials.Clear();
            for (var i = 0; i < chunkCount; ++i) app.Chunk.ChunkMaterials.Add($"board_{Parts[Math.Min(i, Parts.Length - 1)]}");
        }
        Write(mesh, meshDepot);
        var box = cm.BoundingBox;
        Console.Error.WriteLine($"  mesh: {chunkCount} chunks, {cm.Appearances.Count} appearance(s), bounds ({(float)box.Min.X:F3}, {(float)box.Min.Y:F3}, {(float)box.Min.Z:F3}) .. ({(float)box.Max.X:F3}, {(float)box.Max.Y:F3}, {(float)box.Max.Z:F3})");

        // 3. Materials: the template .mi with this part's textures.
        foreach (var part in Parts)
        {
            var mi = Parse(Vanilla(TemplateMi));
            mi.EmbeddedFiles.Clear();  // the template carries its own textures inside; ours are separate files
            var inst = (CMaterialInstance)mi.RootChunk;
            foreach (var kv in inst.Values)
            {
                var key = (string)kv.Key;
                var kind = key switch { "BaseColor" => "d", "Normal" => "n", "Roughness" => "r", "Metalness" => "m", _ => null };
                if (kind != null) kv.Value = new CResourceReference<ITexture>($@"{Dir}\textures\board_{part}_{kind}.xbm");
            }
            Write(mi, $@"{Dir}\board_{part}.mi");
        }

        // 4. Entity: the template's one mesh component now shows the board.
        var ent = Parse(Vanilla(TemplateEnt));
        var et = (entEntityTemplate)ent.RootChunk;
        var meshRef = new CResourceAsyncReference<CMesh>(meshDepot);
        var comps = new List<entMeshComponent>();
        if (et.CompiledData?.Data is RedPackage pkg) comps.AddRange(pkg.Chunks.OfType<entMeshComponent>());
        comps.AddRange(et.Components.OfType<entMeshComponent>());
        foreach (var c in comps) { c.Mesh = meshRef; c.Name = "deckrunner_board"; c.MeshAppearance = "default"; }
        // The dependency list names the mesh too ("Soft"): replace the template's entry with one of the same type and flags.
        if (et.ResolvedDependencies.Count > 0)
        {
            var flags = et.ResolvedDependencies[0].Flags;
            et.ResolvedDependencies.Clear();
            et.ResolvedDependencies.Add(new CResourceAsyncReference<CResource>(meshDepot, flags));
            Console.Error.WriteLine($"  entity dependency: {meshDepot} ({flags})");
        }
        Write(ent, $@"{Dir}\deckrunner_board.ent");
        Console.Error.WriteLine($"  entity: {comps.Count} mesh component reference(s) now point at {meshDepot}");
        return ok ? 0 : 1;
    }

    public static CR2WFile Read(Archive ar, ulong hash, out long size)
    {
        using var ms = new MemoryStream();
        ar.CopyFileToStream(ms, hash, Environment.GetEnvironmentVariable("WK_SEGDECOMP") != null);
        size = ms.Length;
        ms.Position = 0;
        using var r = new CR2WReader(ms);
        return r.ReadFile(out CR2WFile cr2w, true) == EFileReadErrorCodes.NoError ? cr2w : null;
    }

    static void Describe(entEntityTemplate t)
    {
        if (t.CompiledData?.Data is RedPackage pkg)
        {
            foreach (var chunk in pkg.Chunks)
            {
                switch (chunk)
                {
                    case entEntity e: Console.WriteLine($"    entity {e.GetType().Name}"); break;
                    case entIComponent c:
                        var mesh = c switch
                        {
                            entMeshComponent m => (string)m.Mesh.DepotPath,
                            entSkinnedMeshComponent s => (string)s.Mesh.DepotPath,
                            _ => null,
                        };
                        Console.WriteLine($"    component {c.GetType().Name} \"{c.Name}\"{(mesh != null ? " mesh " + mesh : "")}");
                        break;
                }
            }
        }
        else Console.WriteLine("    (no compiled data)");
        Console.WriteLine($"    appearances {t.Appearances.Count}, includes {t.Includes.Count}");
    }
}
