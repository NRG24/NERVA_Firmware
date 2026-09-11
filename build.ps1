# Build (and optionally flash) the ring firmware.
#
#   .\build.ps1            # build (production: no bench instruments)
#   .\build.ps1 -Bench     # build with bench instrumentation
#   .\build.ps1 -Flash     # build, then flash over the Debug Probe
#   .\build.ps1 -Rtt       # build, flash, then attach the RTT viewer
#
# Bench and production differ only by Kconfig, never by edited source. They
# used to be two hand-edited copies of main.c kept in step by hand, and the
# duplicate rotted immediately. See Kconfig and bench.conf.
#
# The app lives outside the NCS workspace, so west runs with its cwd inside
# C:\ncs\v3.4.0 and takes the app path as an argument. BOARD_ROOT has to be
# passed explicitly because sysbuild -- not this app -- is the top-level
# CMake source, so the app directory is not picked up as a board root.
#
# Note: no $ErrorActionPreference = "Stop" here on purpose. Windows
# PowerShell 5.1 wraps native-tool stderr in ErrorRecords, so a Stop
# preference turns ordinary build chatter into a thrown error. Exit codes
# are checked explicitly instead.

param(
	[switch]$Flash,
	[switch]$Rtt,
	[switch]$Bench,
	[int]$Freq          = 1000000,
	[string]$NcsVersion = "v3.4.0",
	[string]$Ncs        = "C:\ncs"
)

$app       = $PSScriptRoot
$nrfutil   = Join-Path $Ncs "tools\nrfutil.exe"
$workspace = Join-Path $Ncs $NcsVersion

# Separate build directories, so switching between the two does not force a
# full rebuild and a bench image can never be left sitting where a
# production one is expected.
if ($Bench) {
	$build = Join-Path $app "build-bench"
	$extra = @("-DBOARD_ROOT=$app", "-DEXTRA_CONF_FILE=bench.conf")
	Write-Output "=== BENCH build: instrumented, not shippable ==="
} else {
	$build = Join-Path $app "build"
	$extra = @("-DBOARD_ROOT=$app")
}

$hex = Join-Path $build "ring-fw\zephyr\zephyr.hex"

& $nrfutil sdk-manager toolchain launch --ncs-version $NcsVersion --chdir $workspace -- `
	west build -b ring_anna/nrf52833 -p always -d $build $app -- @extra

if ($LASTEXITCODE -ne 0 -or -not (Test-Path $hex)) {
	Write-Output "BUILD FAILED"
	exit 1
}

Write-Output "built: $hex"

if ($Flash -or $Rtt) {
	# SWD runs slow because of the series resistors protecting the 1.8V
	# domain from the probe's 3.3V drive. 1 MHz is the default; drop to
	# -Freq 500000 if connects are flaky.
	python -m pyocd flash -t nrf52833 -f $Freq $hex
	if ($LASTEXITCODE -ne 0) {
		Write-Output "FLASH FAILED"
		exit 1
	}
}

if ($Rtt) {
	python -m pyocd rtt -t nrf52833 -f $Freq
}
