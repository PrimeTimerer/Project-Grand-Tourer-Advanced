// The launcher window (borderless, sidebar of pages, in-window dialogs and toasts). The page building blocks are in
// UiKit.cs (shared with the Sakatsuku PC launcher).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using System.Windows.Shell;

namespace GT6Launcher
{
    public interface IPage
    {
        FrameworkElement View { get; }
        void Shown();  // the page is selected (data is read again)
        void Hidden();
    }

    public class DialogAction
    {
        public string Label;
        public Action Run;
        public bool Primary;

        public DialogAction(string label, Action run, bool primary = false)
        {
            Label = label;
            Run = run;
            Primary = primary;
        }
    }

    public class MainWindow : Window
    {
        public static string AppTitle { get { return L.T("GT6 PC 런처"); } }
        public static readonly string[] PageKeys = { "home", "setup", "settings", "mods", "saves", "logs", "tools" };
        public readonly Launcher Launcher;
        readonly Grid root = new Grid();
        readonly Grid host = new Grid();
        readonly Grid overlay = new Grid { Visibility = Visibility.Collapsed };
        readonly StackPanel toasts = new StackPanel { HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, 24, 24) };
        readonly Dictionary<string, RadioButton> navButtons = new Dictionary<string, RadioButton>();
        readonly Dictionary<string, IPage> pages = new Dictionary<string, IPage>();
        readonly Dictionary<string, Func<IPage>> factories = new Dictionary<string, Func<IPage>>();
        public readonly TextBlock UpdateBadge = new TextBlock();
        public ReleaseInfo LatestRelease;
        IPage current;

        public MainWindow(Launcher launcher)
        {
            Launcher = launcher;
            Title = AppTitle;
            Width = 1180;
            Height = 780;
            MinWidth = 980;
            MinHeight = 640;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
            FontFamily = Ui.Res<FontFamily>("UiFont");
            Foreground = Ui.Brush("Text");
            Background = new SolidColorBrush(Color.FromRgb(0x0A, 0x0B, 0x0F));
            ImageSource icon = AppIcon(256);
            if (icon != null)
                Icon = icon;
            WindowStyle = WindowStyle.None;
            WindowChrome.SetWindowChrome(this, new WindowChrome
            {
                CaptionHeight = 44, ResizeBorderThickness = new Thickness(6), GlassFrameThickness = new Thickness(0),
                CornerRadius = new CornerRadius(0), UseAeroCaptionButtons = false,
            });
            // Maximized, a borderless window reaches past the screen by its resize border.
            StateChanged += delegate { root.Margin = WindowState == WindowState.Maximized ? new Thickness(7) : new Thickness(0); };

            root.Children.Add(Glow(0.90, 0.02, Color.FromArgb(0x3A, 0xFF, 0x5A, 0x3D)));
            root.Children.Add(Glow(0.02, 1.0, Color.FromArgb(0x26, 0x4D, 0x7C, 0xFF)));
            var frame = new Grid();
            frame.RowDefinitions.Add(new RowDefinition { Height = new GridLength(44) });
            frame.RowDefinitions.Add(new RowDefinition());
            root.Children.Add(frame);
            frame.Children.Add(TitleBar());
            var body = new Grid();
            Grid.SetRow(body, 1);
            body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(232) });
            body.ColumnDefinitions.Add(new ColumnDefinition());
            frame.Children.Add(body);
            body.Children.Add(Sidebar());
            host.Margin = new Thickness(12, 4, 28, 0);
            Grid.SetColumn(host, 1);
            body.Children.Add(host);
            root.Children.Add(overlay);
            root.Children.Add(toasts);
            Content = root;

            Add("home", "", L.T("홈"), delegate { return new HomePage(this); });
            Add("setup", "", L.T("설치"), delegate { return new SetupPage(this); });
            Add("settings", "", L.T("설정"), delegate { return new SettingsPage(this); });
            Add("mods", "", L.T("모드"), delegate { return new ModsPage(this); });
            Add("saves", "", L.T("세이브"), delegate { return new SavePage(this); });
            Add("logs", "", L.T("로그"), delegate { return new LogPage(this); });
            Add("tools", "", L.T("업데이트 · 도구"), delegate { return new ToolsPage(this); });
            // A new copy without the game files opens on the setup.
            Navigate(SetupRunner.Needed(launcher) ? "setup" : "home");
            // Closing while the setup runs: asked first; the setup stops (what is done stays) before the window goes.
            Closing += delegate(object sender, System.ComponentModel.CancelEventArgs e)
            {
                if (!SetupPage.Running || SetupPage.Closing)
                    return;
                e.Cancel = true;
                Confirm(L.T("설치를 멈추고 닫을까요?"), L.T("지금까지 한 것은 그대로 남고, 다음에 런처에서 설치를 다시 시작하면 멈춘 곳부터 이어집니다."), L.T("멈추고 닫기"), delegate
                {
                    SetupPage.Stop(this, delegate { Close(); });
                });
            };
            // A new release on GitHub: a line in the title bar that opens the tools page (checked once a day).
            PortUpdate.CheckDaily(launcher, delegate(ReleaseInfo info)
            {
                LatestRelease = info;
                if (!info.Newer)
                    return;
                UpdateBadge.Text = L.T("새 버전 ") + info.Tag + L.T(" 있음");
                UpdateBadge.Visibility = Visibility.Visible;
                if (!PortUpdate.AutoEnabled(launcher))
                    return;
                // Automatic: downloaded now, applied the next time the launcher opens.
                UpdateBadge.Text = L.T("새 버전 ") + info.Tag + L.T(" 받는 중...");
                PortUpdate.DownloadInBackground(launcher, info, delegate(string error)
                {
                    if (error != null)
                    {
                        UpdateBadge.Text = L.T("새 버전 ") + info.Tag + L.T(" 있음 (자동으로 받지 못함)");
                        return;
                    }
                    UpdateBadge.Text = L.T("새 버전 ") + info.Tag + L.T(" 받음: 다음 실행 때 적용");
                    Toast(L.T("새 버전 ") + info.Tag + L.T("을 받았습니다. 런처를 다시 열면 적용됩니다."));
                    Refresh();
                });
            });
            Closing += delegate(object sender, System.ComponentModel.CancelEventArgs e)
            {
                if (!e.Cancel && current != null)
                    current.Hidden();
            };
        }

        // The launcher's own icon (icon.ico inside the exe), the picture nearest the asked size.
        public static ImageSource AppIcon(int size)
        {
            try
            {
                using (Stream stream = typeof(MainWindow).Assembly.GetManifestResourceStream("icon.ico"))
                {
                    if (stream == null)
                        return null;
                    var decoder = new IconBitmapDecoder(stream, BitmapCreateOptions.None, BitmapCacheOption.OnLoad);
                    BitmapFrame best = decoder.Frames.OrderBy(f => Math.Abs(f.PixelWidth - size)).ThenByDescending(f => f.Format.BitsPerPixel).First();
                    best.Freeze();
                    return best;
                }
            }
            catch (Exception)
            {
                return null;
            }
        }

        static Rectangle Glow(double x, double y, Color color)
        {
            var brush = new RadialGradientBrush { Center = new Point(x, y), GradientOrigin = new Point(x, y), RadiusX = 0.65, RadiusY = 0.75 };
            brush.GradientStops.Add(new GradientStop(color, 0));
            brush.GradientStops.Add(new GradientStop(Color.FromArgb(0, color.R, color.G, color.B), 1));
            return new Rectangle { Fill = brush, IsHitTestVisible = false };
        }

        UIElement TitleBar()
        {
            var bar = new DockPanel { LastChildFill = false };
            var logo = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(20, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            ImageSource icon = AppIcon(48);
            if (icon != null)
                logo.Children.Add(new Image { Source = icon, Width = 22, Height = 22 });
            else
                logo.Children.Add(new Border { Width = 22, Height = 22, CornerRadius = new CornerRadius(6), Background = Ui.Brush("PlayGradient") });
            logo.Children.Add(new TextBlock { Text = AppTitle, Margin = new Thickness(10, 0, 0, 0), FontSize = 13, FontWeight = FontWeights.SemiBold, Foreground = Ui.Brush("SubText"), VerticalAlignment = VerticalAlignment.Center });
            DockPanel.SetDock(logo, Dock.Left);
            bar.Children.Add(logo);
            Button close = Chrome("", "CloseButton", delegate { Close(); });
            Button maximize = Chrome("", "ChromeButton", delegate { WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized; });
            Button minimize = Chrome("", "ChromeButton", delegate { WindowState = WindowState.Minimized; });
            StateChanged += delegate { maximize.Content = WindowState == WindowState.Maximized ? "" : ""; };
            foreach (Button button in new[] { close, maximize, minimize })
            {
                DockPanel.SetDock(button, Dock.Right);
                bar.Children.Add(button);
            }
            UpdateBadge.Visibility = Visibility.Collapsed;
            UpdateBadge.Foreground = Ui.Brush("Accent");
            UpdateBadge.FontSize = 12;
            UpdateBadge.VerticalAlignment = VerticalAlignment.Center;
            UpdateBadge.Margin = new Thickness(0, 0, 16, 0);
            UpdateBadge.Cursor = System.Windows.Input.Cursors.Hand;
            UpdateBadge.MouseLeftButtonUp += delegate { Navigate("tools"); };
            WindowChrome.SetIsHitTestVisibleInChrome(UpdateBadge, true);
            DockPanel.SetDock(UpdateBadge, Dock.Right);
            bar.Children.Add(UpdateBadge);
            return bar;
        }

        Button Chrome(string glyph, string style, Action click)
        {
            var button = new Button { Content = glyph, Style = Ui.Style(style), Height = 44 };
            button.Click += delegate { click(); };
            WindowChrome.SetIsHitTestVisibleInChrome(button, true);
            return button;
        }

        readonly StackPanel nav = new StackPanel { Margin = new Thickness(0, 12, 0, 0) };

        UIElement Sidebar()
        {
            var side = new DockPanel();
            var footer = new StackPanel { Margin = new Thickness(28, 0, 16, 20) };
            footer.Children.Add(Ui.Text("Gran Turismo 6 (" + GameFiles.TitleId + L.T(" 1.22)\n네이티브 PC 포트"), "Caption"));
            var note = Ui.Text(L.T("게임 데이터 미포함\n본인 PS3 게임 파일 필요"), "Caption");
            note.Foreground = Ui.Brush("Muted");
            note.Margin = new Thickness(0, 4, 0, 0);
            footer.Children.Add(note);
            DockPanel.SetDock(footer, Dock.Bottom);
            side.Children.Add(footer);
            side.Children.Add(nav);
            return side;
        }

        void Add(string key, string glyph, string label, Func<IPage> factory)
        {
            factories[key] = factory;
            var content = new StackPanel { Orientation = Orientation.Horizontal };
            content.Children.Add(new TextBlock { Text = glyph, Style = Ui.Style("Icon"), Width = 28 });
            content.Children.Add(new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center });
            var button = new RadioButton { Content = content, Style = Ui.Style("Nav"), GroupName = "nav" };
            button.Checked += delegate { Show(key); };
            navButtons[key] = button;
            nav.Children.Add(button);
        }

        public void Navigate(string key)
        {
            if (navButtons[key].IsChecked == true)
                Show(key);
            else
                navButtons[key].IsChecked = true;
        }

        void Show(string key)
        {
            if (current != null)
                current.Hidden();
            IPage page;
            if (!pages.TryGetValue(key, out page))
                pages[key] = page = factories[key]();
            current = page;
            host.Children.Clear();
            host.Children.Add(page.View);
            page.Shown();
            page.View.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(180)));
        }

        public void Refresh()
        {
            if (current != null)
                current.Shown();
        }

        // Starts the game (the window hides while it runs and comes back when it ends).
        public void Play()
        {
            if (current != null)
                current.Hidden();
            string problem = Launcher.Runner.Start();
            if (problem != null)
            {
                ShowProblem(problem);
                Refresh();
            }
            else
                Hide();
        }

        public void ShowProblem(string problem)
        {
            Dialog("", Ui.Brush("Warn"), L.T("게임을 시작할 수 없습니다"), problem, new DialogAction(L.T("확인"), null, true));
        }

        public void GameFinished(RunResult result)
        {
            Show();
            Activate();
            Refresh();
            Diagnosis d = result.Diagnosis;
            if (d == null)
            {
                Toast(L.T("플레이 시간 ") + result.Ran.ToString(@"h\:mm\:ss"));
                return;
            }
            Dialog("", Ui.Brush("Bad"), d.Title, d.Detail + "\n\n" + d.Advice,
                   new DialogAction(L.T("로그 보기"), delegate { Navigate("logs"); }, true), new DialogAction(L.T("닫기"), null));
        }

        // A dialog inside the window (dark backdrop, card in the middle).
        public void Dialog(string glyph, Brush glyphBrush, string title, string message, params DialogAction[] actions)
        {
            overlay.Children.Clear();
            overlay.Background = new SolidColorBrush(Color.FromArgb(0xB0, 0x03, 0x04, 0x07));
            var card = new Border { Style = Ui.Style("Card"), MaxWidth = 560, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Padding = new Thickness(28), Background = new SolidColorBrush(Color.FromRgb(0x15, 0x18, 0x1F)) };
            card.Effect = new DropShadowEffect { Color = Colors.Black, BlurRadius = 40, ShadowDepth = 0, Opacity = 0.6 };
            var stack = new StackPanel();
            var head = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, 14) };
            head.Children.Add(new TextBlock { Text = glyph, Style = Ui.Style("Icon"), FontSize = 22, Foreground = glyphBrush, Margin = new Thickness(0, 0, 12, 0) });
            var titleText = Ui.Text(title, "H2");
            titleText.Margin = new Thickness(0);
            titleText.VerticalAlignment = VerticalAlignment.Center;
            head.Children.Add(titleText);
            stack.Children.Add(head);
            stack.Children.Add(Ui.Text(message, "Body"));
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 22, 0, 0) };
            foreach (DialogAction action in actions)
            {
                DialogAction a = action;
                buttons.Children.Add(Ui.Button(a.Label, delegate
                {
                    overlay.Visibility = Visibility.Collapsed;
                    if (a.Run != null)
                        a.Run();
                }, a.Primary ? "AccentButton" : null, null, new Thickness(8, 0, 0, 0)));
            }
            stack.Children.Add(buttons);
            card.Child = stack;
            overlay.Children.Add(card);
            overlay.Visibility = Visibility.Visible;
            card.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(160)));
        }

        public void Confirm(string title, string message, string yes, Action run)
        {
            Dialog("", Ui.Brush("Accent"), title, message, new DialogAction(yes, run, true), new DialogAction(L.T("취소"), null));
        }

        public void Error(string title, Exception error)
        {
            Dialog("", Ui.Brush("Bad"), title, error.Message, new DialogAction(L.T("확인"), null, true));
        }

        public void Toast(string text)
        {
            var toast = new Border
            {
                Background = new SolidColorBrush(Color.FromRgb(0x1B, 0x1F, 0x28)), BorderBrush = Ui.Brush("Accent"), BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(12), Padding = new Thickness(16, 10, 16, 10), Margin = new Thickness(0, 8, 0, 0),
                Child = new TextBlock { Text = text, Foreground = Ui.Brush("Text"), FontSize = 13 },
                Effect = new DropShadowEffect { Color = Color.FromRgb(0xFF, 0x6A, 0x3D), BlurRadius = 18, ShadowDepth = 0, Opacity = 0.35 },
            };
            toasts.Children.Add(toast);
            var fade = new DoubleAnimation(1, 0, TimeSpan.FromMilliseconds(500)) { BeginTime = TimeSpan.FromSeconds(2.6) };
            fade.Completed += delegate { toasts.Children.Remove(toast); };
            toast.BeginAnimation(OpacityProperty, fade);
        }
    }
}
