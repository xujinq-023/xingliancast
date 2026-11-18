$f = 'D:\DSH-Workplace\XingLianCast\sender-pc\ui\ProjectorUi.cs'
$enc = New-Object System.Text.UTF8Encoding($true)
$c = [IO.File]::ReadAllText($f, [Text.Encoding]::UTF8)

# ---------- A. 每秒信标不再触碰控件（修复闪烁/下拉无法展开） ----------
$c = $c.Replace("            if (SameDeviceList(devices)) { UpdateDeviceHint(); return; }",
"            // 列表没变化：直接返回，绝不碰任何控件（之前每秒调 UpdateDeviceHint 会重排界面，
            // 导致窗口每秒闪一下、所有下拉框刚展开就被打断）
            if (SameDeviceList(devices)) return;
            if (_cboDevice != null && _cboDevice.DroppedDown) return;   // 下拉展开期间不接受刷新")

# ---------- B. 未发现电视：提示放进接收端下拉框内，红色 ----------
$c = $c.Replace("        void UpdateDeviceHint()
        {", "        void UpdateDeviceHint()
        {
            // 提示直接显示在接收端下拉框里（红色），不再占用单独一行
")

$old = "            bool show = !found && !_searching;
            if (show) {
                _lblDeviceHint.Text = ""未发现电视端，请确认开启后重试"";"
# 用变量避免引号转义问题
$q = [char]34
$oldBlock = @"
            bool show = !found && !_searching;
"@
if ($c.Contains($oldBlock)) {
  # 在 UpdateDeviceHint 里追加：按有无设备设置下拉框文字颜色
  $c = $c.Replace($oldBlock, @"
            bool show = !found && !_searching;
            if (_cboDevice != null) _cboDevice.ForeColor = found ? T.Fg : T.Err;   // 无设备时红字
"@)
}

# ---------- C. 接收端下拉框挪到第一行；行 3 高度归零 ----------
$c = $c.Replace("            grid.Controls.Add(_cboDevice, 1, 3);", "            grid.Controls.Add(_cboDevice, 3, 0);")
$c = $c.Replace("            grid.Controls.Add(_btnRefresh, 2, 3);", "            grid.Controls.Add(_btnRefresh, 4, 0);")
$c = $c.Replace("            grid.RowCount = 6;", "            grid.RowCount = 6;")
$c = $c.Replace("            for (int i = 0; i < 4; i++) grid.RowStyles.Add(new RowStyle(SizeType.Absolute, T.I(44)));",
"            for (int i = 0; i < 4; i++) grid.RowStyles.Add(new RowStyle(SizeType.Absolute, i == 3 ? 0 : T.I(44)));   // 第 4 行(接收端)已上移，高度归零")
$c = $c.Replace("            grid.ColumnCount = 4;", "            grid.ColumnCount = 5;")

# ---------- D. 指标改名 + 第 10 格显示总发送流量 ----------
$c = $c.Replace('"电脑帧率", "编码耗时/帧", "音频带宽", "网络流量", "电脑丢包/秒",',
'"电脑帧率", "编码耗时/帧", "音频带宽", "实时流量", "电脑丢包/秒",')
$c = $c.Replace('"电视帧率", "采集卡顿", "限帧丢弃", "网络延迟", "电视丢包/秒"',
'"电视帧率", "采集卡顿", "限帧丢弃", "网络延迟", "总发送流量"')
$c = $c.Replace('            _mv[9].Set(tv ? Field(line, "tvlost=", "") : "—");',
'            UpdateTotalTraffic(line);        // 第 10 格改为「总发送流量」')

$c = $c.Replace('            RefreshTrafficLabel();
        }', @'
            if (_mv[9] != null) _mv[9].Set(Human(_txTotalBytes));
        }
'@)

$c = $c -replace 'v1\.0\.4[0-9]', 'v1.0.47'
[IO.File]::WriteAllText($f, $c, $enc)
'patched'
