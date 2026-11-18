using System.Reflection;
static class Members
{
    public static void Dump(string typeName)
    {
        foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
        {
            var t = asm.GetType(typeName);
            if (t == null) continue;
            Console.WriteLine($"== {t.FullName}");
            if (t.IsEnum) { Console.WriteLine("  " + string.Join(", ", Enum.GetNames(t))); continue; }
            foreach (var c in t.GetConstructors()) Console.WriteLine($"  ctor({string.Join(", ", c.GetParameters().Select(p => p.ParameterType.Name + " " + p.Name))})");
            foreach (var m in t.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly))
                Console.WriteLine($"  {(m.IsStatic ? "static " : "")}{m.ReturnType.Name} {m.Name}({string.Join(", ", m.GetParameters().Select(p => p.ParameterType.Name + " " + p.Name))})");
            foreach (var f in t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly)) Console.WriteLine($"  field {(f.IsStatic ? "static " : "")}{f.FieldType.Name} {f.Name}");
            foreach (var p in t.GetProperties(BindingFlags.Public | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly)) Console.WriteLine($"  prop {p.PropertyType.Name} {p.Name}");
        }
    }
}
