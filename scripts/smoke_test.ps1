$ErrorActionPreference = "Stop"
cmake --build build
ctest --test-dir build --output-on-failure
& .\build\hashjoin.exe --r-size 20000 --s-size 40000 --key-range 20000 --skew 1.0 --seed 7 --threads 4 --repetitions 2 --warmups 1 --diagnostics --csv build\smoke.csv
if ($LASTEXITCODE -ne 0) { throw "Smoke benchmark failed" }
Write-Host "Smoke output: build\smoke.csv"
