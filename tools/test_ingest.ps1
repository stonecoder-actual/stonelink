# In-memory pipeline tests only; never opens physical SD cards or serial ports.
param(
    [string]$Compiler = '.pio/host-tools/tcc/tcc.exe',
    [string]$Framework = "$env:USERPROFILE/.platformio/packages/framework-espidf"
)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $taskCompiler = (Resolve-Path -LiteralPath $Compiler).Path
    $taskFramework = (Resolve-Path -LiteralPath $Framework).Path
    New-Item -ItemType Directory -Force .pio | Out-Null
    $taskSource = [IO.File]::ReadAllText((Join-Path $PWD 'src/local_storage.c'))
    $taskStart = $taskSource.IndexOf('static bool joined(')
    if ($taskStart -lt 0) { throw 'Production storage test boundary not found' }
    [IO.File]::WriteAllText((Join-Path $PWD '.pio/ingest_under_test.inc'), $taskSource.Substring($taskStart), [Text.UTF8Encoding]::new($false))
    $taskRom = Join-Path $taskFramework 'components/esp_rom/linux/esp_rom_crc.c'
    $taskInclude = '-I' + (Join-Path $taskFramework 'components/esp_rom/include')
    & $taskCompiler -Itests/pipeline_stubs -Itests/queue_stubs -Itests/io_stubs -Itests/host_stubs -Isrc $taskInclude -o .pio/ingest_pipeline_test.exe tests/ingest_pipeline_test.c $taskRom
    if ($LASTEXITCODE -ne 0) { throw 'Pipeline test compilation failed' }
    & ./.pio/ingest_pipeline_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Pipeline test failed' }
    & $taskCompiler -Itests/io_stubs -Isrc $taskInclude -o .pio/media_io_test.exe tests/media_io_test.c $taskRom
    if ($LASTEXITCODE -ne 0) { throw 'CRC/yield test compilation failed' }
    & ./.pio/media_io_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'CRC/yield test failed' }
    & $taskCompiler -Isrc -run tests/power_cycle_test.c
    if ($LASTEXITCODE -ne 0) { throw 'Power policy test failed' }
} finally {
    Pop-Location
}
