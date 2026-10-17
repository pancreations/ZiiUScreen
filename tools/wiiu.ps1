# Reusable console interface. Configuration and captures stay in .local/wiiu/workbench.
& py -3 "$PSScriptRoot\wiiu-workbench\workbench.py" @args
exit $LASTEXITCODE
