param([int]$AppId)
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class ShellProbe {
 public delegate bool EnumWin(IntPtr hwnd,IntPtr ctx);
 [StructLayout(LayoutKind.Sequential)] public struct Rect {public int left,top,right,bottom;}
 [StructLayout(LayoutKind.Sequential)] public struct IconId { public uint cbSize; public IntPtr hWnd; public uint id; public Guid guid; }
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cl,string title);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr root,IntPtr prev,string cl,string title);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h,EnumWin func,IntPtr arg);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,StringBuilder buf,int len);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect r);
 [DllImport("shell32.dll")] public static extern int Shell_NotifyIconGetRect(ref IconId id,out Rect r);
 public static void Inventory(int pid) {
  var fg=GetForegroundWindow();uint fgpid;GetWindowThreadProcessId(fg,out fgpid);
  var cl=new StringBuilder(100);GetClassName(fg,cl,100);
  Console.WriteLine("Foreground class "+cl+" process "+ System.Diagnostics.Process.GetProcessById((int)fgpid).ProcessName);
  var tray=FindWindow("Shell_TrayWnd",null);
  Console.WriteLine("Explorer taskbar " + tray);
  EnumChildWindows(tray,(h,arg)=>{var name=new StringBuilder(100);GetClassName(h,name,100);Rect r;GetWindowRect(h,out r);Console.WriteLine("TASKBAR "+name+" "+r.left+","+r.top+","+r.right+","+r.bottom);return true;},IntPtr.Zero);
  IntPtr previous=IntPtr.Zero;
  while((previous=FindWindowEx(IntPtr.Zero,previous,null,"QTrayIconMessageWindow"))!=IntPtr.Zero) {
   uint id;GetWindowThreadProcessId(previous,out id);if(id!=pid)continue;
   var icon=new IconId{cbSize=(uint)Marshal.SizeOf(typeof(IconId)),hWnd=previous,id=0};Rect r;int hr=Shell_NotifyIconGetRect(ref icon,out r);
   Console.WriteLine("TRAY HWND "+previous+" HRESULT "+hr.ToString("X8")+" rect "+r.left+","+r.top+","+r.right+","+r.bottom);
  }
 }
}
"@
[ShellProbe]::Inventory($AppId)
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
$bar = [System.Windows.Automation.AutomationElement]::FromHandle([ShellProbe]::FindWindow('Shell_TrayWnd',$null))
$bar.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition) | ForEach-Object {
  if ($_.Current.IsOffscreen -eq $false) { Write-Output "UIA $($_.Current.ControlType.ProgrammaticName) [$($_.Current.Name)] id [$($_.Current.AutomationId)] class [$($_.Current.ClassName)] bounds $($_.Current.BoundingRectangle)" }
}
