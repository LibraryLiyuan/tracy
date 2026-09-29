using UnityEditor;
using UnityEngine;

// Readiness marker for an isolated empty Editor project. No game project is modified.
[InitializeOnLoad]
public static class CaptureGuiReadiness
{
    static CaptureGuiReadiness()
    {
        EditorApplication.delayCall += () => Debug.Log("CAPTURE_GUI_EDITOR_READY");
    }
}
