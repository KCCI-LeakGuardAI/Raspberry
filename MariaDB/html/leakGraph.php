<?php
/* -----------------------------------------------------------------------------
 *  leakGraph.php — 시간대별 누수 통계 (leakdb)
 *  배치: sudo cp leakGraph.php /var/www/html/   ->  http://<Pi주소>/leakGraph.php
 *  최근 N일만 보기: leakGraph.php?days=7
 * -------------------------------------------------------------------------- */
    $conn = mysqli_connect("localhost", "iot", "pwiot");
    mysqli_select_db($conn, "leakdb");

    $days  = isset($_GET['days']) ? intval($_GET['days']) : 0;
    $where = ($days > 0) ? "ts >= NOW() - INTERVAL $days DAY" : "1";

    /* 시간대별: 누수 시간 비중 + 평균 면적
       "누수" = state BETWEEN 1 AND 3 (SMALL_LEAK/WARNING/DANGER). 4 는 센서오류.
       state=1 만 세면 심한 누수일수록 통계에서 빠진다. */
    $query = "SELECT hh,
                     COUNT(*)                                                  AS samples,
                     100*SUM(state BETWEEN 1 AND 3)/COUNT(*)                   AS leak_pct,
                     AVG(area)/100.0                                           AS avg_area,
                     AVG(CASE WHEN state BETWEEN 1 AND 3 THEN area END)/100.0  AS avg_leak_area,
                     100*SUM(state=3)/COUNT(*)                                 AS danger_pct
              FROM leak_sample WHERE $where GROUP BY hh";
    $result = mysqli_query($conn, $query);

    /* 0~23시를 빈칸 없이 채운다 (데이터 없는 시간대도 축에 나와야 함) */
    $leak = array_fill(0, 24, 0.0);
    $avga = array_fill(0, 24, 0.0);
    $lka  = array_fill(0, 24, 0.0);
    $dng  = array_fill(0, 24, 0.0);
    $cnt  = array_fill(0, 24, 0);
    if ($result) {
        while ($row = mysqli_fetch_array($result)) {
            $h = intval($row['hh']);
            $cnt[$h]  = intval($row['samples']);
            $leak[$h] = floatval($row['leak_pct']);
            $avga[$h] = floatval($row['avg_area']);
            $lka[$h]  = floatval($row['avg_leak_area']);
            $dng[$h]  = floatval($row['danger_pct']);
        }
    }

    /* 시간대별 누수 발생 횟수 (전이 기준) */
    $ev = array_fill(0, 24, 0);
    $q2 = "SELECT hh, SUM(fsm IN (3,4)) AS n FROM leak_event WHERE $where GROUP BY hh";
    $r2 = mysqli_query($conn, $q2);
    if ($r2) {
        while ($row = mysqli_fetch_array($r2))
            $ev[intval($row['hh'])] = intval($row['n']);
    }

    $data = array(array('시간', '누수시간 비중(%)', 'DANGER 비중(%)', '평균 면적(%)', '누수 발생(회)'));
    for ($h = 0; $h < 24; $h++) {
        array_push($data, array(sprintf("%02d시", $h),
                                round($leak[$h], 2), round($dng[$h], 2),
                                round($avga[$h], 3), $ev[$h]));
    }

    $options = array(
        'title'  => 'Leak Guard — 시간대별 누수 통계' . ($days > 0 ? " (최근 {$days}일)" : ' (전체)'),
        'width'  => 1100, 'height' => 420,
        'seriesType' => 'bars',
        'series' => array('3' => array('type' => 'line', 'targetAxisIndex' => 1)),
        'vAxes'  => array(array('title' => '%'), array('title' => '회')),
        'hAxis'  => array('title' => '시간대')
    );
?>

<meta charset="utf-8">
<script src="//www.google.com/jsapi"></script>
<script>
var data    = <?= json_encode($data) ?>;
var options = <?= json_encode($options) ?>;

google.load('visualization', '1.0', {'packages':['corechart']});
google.setOnLoadCallback(function() {
    var chart = new google.visualization.ComboChart(document.querySelector('#chart_div'));
    chart.draw(google.visualization.arrayToDataTable(data), options);
});
</script>
<div id="chart_div"></div>

<h3>시간대별 상세</h3>
<table border="1" cellpadding="4" style="border-collapse:collapse">
<tr><th>시간</th><th>샘플</th><th>누수시간 비중(%)</th><th>DANGER 비중(%)</th>
    <th>평균 면적(%)</th><th>누수 중 평균 면적(%)</th><th>누수 발생(회)</th></tr>
<?php for ($h = 0; $h < 24; $h++): ?>
<tr>
  <td><?= sprintf("%02d시", $h) ?></td>
  <td align="right"><?= $cnt[$h] ?></td>
  <td align="right"><?= number_format($leak[$h], 2) ?></td>
  <td align="right"><?= number_format($dng[$h],  2) ?></td>
  <td align="right"><?= number_format($avga[$h], 3) ?></td>
  <td align="right"><?= number_format($lka[$h],  3) ?></td>
  <td align="right"><?= $ev[$h] ?></td>
</tr>
<?php endfor; ?>
</table>
