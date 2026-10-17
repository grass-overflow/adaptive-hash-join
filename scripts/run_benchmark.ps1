param(
    [int]$Size = 100000,
    [double]$Skew = 1.25,
    [int]$Seed = 11,
    [int]$Threads = 4,
    [int]$Repetitions = 3
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path results | Out-Null
$output = "results\benchmark_size${Size}_skew${Skew}_seed${Seed}.csv"
& .\build\hashjoin.exe --r-size $Size --s-size $Size --key-range 1000000 --skew $Skew --seed $Seed --threads $Threads --policy all --warmups 1 --repetitions $Repetitions --diagnostics --csv $output
if ($LASTEXITCODE -ne 0) { throw "Benchmark failed" }
Write-Host "Wrote $output"
