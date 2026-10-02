namespace LuminexManager;

// Keep disabled labels readable instead of using the native dark disabled text.
internal sealed class SquareButton : Button
{
    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        if (Enabled) return;

        e.Graphics.Clear(ControlPaint.Dark(BackColor, 0.15f));
        if (FlatAppearance.BorderSize > 0)
        {
            using var pen = new Pen(FlatAppearance.BorderColor.IsEmpty ? Color.White : FlatAppearance.BorderColor,
                FlatAppearance.BorderSize);
            e.Graphics.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
        }
        TextRenderer.DrawText(e.Graphics, Text, Font, ClientRectangle, Color.White,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.WordBreak);
    }
}
