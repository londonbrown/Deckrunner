// Stage C3 graph patch: splice Skate's pose into V's animation graph (player_base.animgraph), lab only.
//
//   Output -> Blend2(weight = variable dkr_weight)
//               first:  the graph's original output (V's normal animation)
//               second: SetBoneOrientation x 24 (model space, one quaternion variable per bone)
//                       over SetBonePosition(Hips, vector variable dkr_p_Hips) over the original output
//
// Every new variable defaults to "no change" (dkr_weight 0), so V animates normally until the plugin turns the weight
// up. The chain applies parents before children (the innermost node is Hips), because a model-space orientation set on
// a parent would otherwise move a child that was already set. Links in the graph are weak handles: the graph owns its
// nodes through nodesToInit, so every new node is added there too.
using System.Reflection;
using WolvenKit.RED4.Archive.CR2W;
using WolvenKit.RED4.Archive.IO;
using WolvenKit.RED4.Types;

static class Patch
{
    // Parents before children (the rig's order); the plugin sets the variables in the same order and by the same names.
    public static readonly string[] Bones =
    {
        "Hips", "Spine", "Spine1", "Spine2", "Spine3", "Neck", "Neck1", "Head",
        "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "RightShoulder", "RightArm", "RightForeArm", "RightHand",
        "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase", "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase",
    };
    public const string Weight = "dkr_weight", HipsPosition = "dkr_p_Hips";
    public static string QuatVar(string bone) => "dkr_q_" + bone;

    public static int Run(string[] args)
    {
        if (args.Length < 3) { Console.Error.WriteLine("usage: patch-skate <in.animgraph> <out.animgraph>"); return 2; }
        CR2WFile cr2w;
        using (var fs = File.OpenRead(args[1]))
        using (var r = new CR2WReader(fs))
            if (r.ReadFile(out cr2w, true) != EFileReadErrorCodes.NoError || cr2w == null) { Console.Error.WriteLine("not a readable CR2W file"); return 1; }
        if (cr2w.RootChunk is not animAnimGraph g) { Console.Error.WriteLine($"root is {cr2w.RootChunk?.GetType().Name}, not animAnimGraph"); return 1; }

        var owned = g.NodesToInit.Select(h => h.Chunk).ToList();
        if (owned.OfType<animAnimNode_QuaternionVariable>().Any(n => ((string)n.VariableName ?? "").StartsWith("dkr_"))) { Console.Error.WriteLine("already patched"); return 1; }
        // Every state of every state machine has its own output node; the graph's own output is the one the root holds.
        if (g.RootNode.Chunk is not animAnimNode_Root root) { Console.Error.WriteLine("no root node"); return 1; }
        var outputs = root.Nodes.Select(h => h.Chunk).OfType<animAnimNode_Output>().ToList();
        if (outputs.Count != 1) { Console.Error.WriteLine($"the root holds {outputs.Count} output nodes; expected 1"); return 1; }
        var output = outputs[0];
        var original = output.Node.Node.GetValue() as animAnimNode_Base;
        if (original == null) { Console.Error.WriteLine("the output node has no input"); return 1; }
        Console.Error.WriteLine($"output feeds {original.GetType().Name}; {owned.Count} owned nodes before");

        var added = new List<animAnimNode_Base>();
        T Add<T>(T n) where T : animAnimNode_Base { n.Id = uint.MaxValue; n.DebugName = "None"; added.Add(n); return n; }

        // Inner part of the chain: Hips position, then each bone's orientation.
        var hipsVar = Add(new animAnimNode_VectorVariable { VariableName = HipsPosition });
        var setHips = Add(new animAnimNode_SetBonePosition { Bone = new animTransformIndex { Name = "Hips" } });
        setHips.InputLink = PoseLink(original);
        setHips.PositionMs = Link<animVectorLink>(hipsVar);
        animAnimNode_Base top = setHips;
        foreach (var bone in Bones)
        {
            var q = Add(new animAnimNode_QuaternionVariable { VariableName = QuatVar(bone) });
            var set = Add(new animAnimNode_SetBoneOrientation { Bone = new animTransformIndex { Name = bone } });
            set.InputLink = PoseLink(top);
            set.OrientationMs = Link<animQuaternionLink>(q);
            top = set;
        }
        var weight = Add(new animAnimNode_FloatVariable { VariableName = Weight });
        var blend = Add(new animAnimNode_Blend2 { MinInputValue = 0f, MaxInputValue = 1f, TimeWarpingEnabled = false });
        blend.FirstInputNode = PoseLink(original);
        blend.SecondInputNode = PoseLink(top);
        blend.WeightNode = Link<animFloatLink>(weight);
        output.Node = PoseLink(blend);

        // Own the new nodes: the blend and its weight right after the output (the graph lists nodes depth first, a node's
        // weight before its inputs), the chain at the end (its innermost input is the original output, already listed).
        var at = g.NodesToInit.Select(h => h.Chunk).ToList().IndexOf(output) + 1;
        g.NodesToInit.Insert(at, new CHandle<animAnimNode_Base>(blend));
        g.NodesToInit.Insert(at + 1, new CHandle<animAnimNode_Base>(weight));
        for (var i = added.Count - 1; i >= 0; --i)
            if (added[i] != blend && added[i] != weight) g.NodesToInit.Add(new CHandle<animAnimNode_Base>(added[i]));

        // Variables, appended (the container is not sorted; nodes find variables by name).
        var vars = g.Variables.Chunk;
        vars.FloatVariables.Add(new CHandle<animAnimVariableFloat>(new animAnimVariableFloat { Name = Weight, Default = 0f, Value = 0f, Min = 0f, Max = 1f }));
        vars.VectorVariables.Add(new CHandle<animAnimVariableVector>(new animAnimVariableVector
        {
            Name = HipsPosition,
            Default = new Vector4 { X = 0, Y = 0, Z = 1, W = 1 },
            Min = new Vector4 { X = -100, Y = -100, Z = -100, W = 1 },
            Max = new Vector4 { X = 100, Y = 100, Z = 100, W = 1 },
            X = 0, Y = 0, Z = 1, W = 1,
        }));
        foreach (var bone in Bones)
            vars.QuaternionVariables.Add(new CHandle<animAnimVariableQuaternion>(new animAnimVariableQuaternion
            {
                Name = QuatVar(bone), Default = new Quaternion { I = 0, J = 0, K = 0, R = 1 }, Roll = 0, Pitch = 0, Yaw = 0,
            }));

        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(args[2]))!);
        using (var fs = File.Create(args[2]))
        using (var w = new CR2WWriter(fs)) w.WriteFile(cr2w);
        Console.Error.WriteLine($"added {added.Count} nodes ({g.NodesToInit.Count} owned), {Bones.Length} quaternion + 1 vector + 1 float variables -> {args[2]} ({new FileInfo(args[2]).Length} bytes)");
        return 0;
    }

    static animPoseLink PoseLink(animAnimNode_Base n) => Link<animPoseLink>(n);

    // Each link type holds a weak handle of its own node type (CWeakHandle<animAnimNode_Base>, ..._QuaternionValue, ...).
    static T Link<T>(animAnimNode_Base n) where T : new()
    {
        var link = new T();
        var prop = typeof(T).GetProperty("Node", BindingFlags.Public | BindingFlags.Instance)!;
        prop.SetValue(link, System.Activator.CreateInstance(prop.PropertyType, n));
        return link;
    }
}
