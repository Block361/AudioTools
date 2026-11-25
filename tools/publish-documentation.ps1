# Publishes the repository-owned documentation set without duplicating existing Docs issues.
param([switch]$Publish)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskDocs = Join-Path $taskRoot 'docs/github-issues'
$taskManifestPath = Join-Path $taskDocs 'manifest.json'
$taskManifest = Get-Content -LiteralPath $taskManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$taskUtf8 = [Text.UTF8Encoding]::new($false)
function Save-Manifest {
    [IO.File]::WriteAllText($taskManifestPath, ($taskManifest | ConvertTo-Json -Depth 10) + "`n", $taskUtf8)
}
function Invoke-Gh([string[]]$Arguments) {
    $taskOutput = & gh @Arguments
    if ($LASTEXITCODE -ne 0) { throw 'GitHub operation failed; inspect remote state before retrying creation.' }
    return ($taskOutput -join "`n")
}
if (-not $Publish) {
    $taskManifest.issues | Select-Object title,file,url
    Write-Output 'Preview only. Use -Publish to create/update issues and verify their cross-links.'
    exit 0
}
$taskPreviousToken = $env:GH_TOKEN
$taskPreviousPrompt = $env:GIT_TERMINAL_PROMPT
$taskPreviousInteractive = $env:GCM_INTERACTIVE
$taskTemporary = Join-Path ([IO.Path]::GetTempPath()) ('OpenAoIP-docs-' + [guid]::NewGuid().ToString('N'))
try {
    if (-not $env:GH_TOKEN) {
        # Reuse the authorized Git login; never print or persist the credential.
        $env:GIT_TERMINAL_PROMPT = '0'
        $env:GCM_INTERACTIVE = 'Never'
        $taskCredential = ("protocol=https`nhost=github.com`n`n") | git -C $taskRoot -c "safe.directory=$($taskRoot.Replace('\','/'))" credential fill
        if ($LASTEXITCODE -ne 0) { throw 'No stored GitHub credential available.' }
        $taskPassword = @($taskCredential | Where-Object { $_.StartsWith('password=') })
        if ($taskPassword.Count -ne 1) { throw 'No GitHub access token available.' }
        $env:GH_TOKEN = $taskPassword[0].Substring(9)
        $taskCredential = $null; $taskPassword = $null
    }
    $taskRepository = $taskManifest.repository
    $taskLabel = $taskManifest.label
    $taskExisting = @(Invoke-Gh @('issue','list','--repo',$taskRepository,'--state','all','--limit','1000','--json','number,title,url') | ConvertFrom-Json)
    $taskLabels = Invoke-Gh @('label','list','--repo',$taskRepository,'--limit','100','--json','name') | ConvertFrom-Json
    if ($taskLabel -notin $taskLabels.name) {
        Invoke-Gh @('label','create',$taskLabel,'--repo',$taskRepository,'--description','Project documentation','--color','0075ca') | Out-Null
    }
    New-Item -ItemType Directory -Path $taskTemporary | Out-Null
    $taskManifest.publicationStatus = 'partial'
    foreach ($taskIssue in $taskManifest.issues) {
        $taskMatches = @($taskExisting | Where-Object { $_.title -eq $taskIssue.title })
        if ($taskMatches.Count -gt 1) { throw "Multiple issues match: $($taskIssue.title)" }
        if ($taskMatches.Count -eq 1) {
            $taskUrl = $taskMatches[0].url
            $taskNumber = $taskMatches[0].number
        } elseif ($taskIssue.PSObject.Properties['url']) {
            throw "Recorded issue no longer matches the title; inspect before publishing: $($taskIssue.url)"
        } else {
            $taskBodyPath = Join-Path $taskTemporary ($taskIssue.key + '.md')
            $taskBody = [IO.File]::ReadAllText((Join-Path $taskDocs $taskIssue.file))
            [IO.File]::WriteAllText($taskBodyPath, $taskBody, $taskUtf8)
            $taskUrl = Invoke-Gh @('issue','create','--repo',$taskRepository,'--title',$taskIssue.title,'--label',$taskLabel,'--body-file',$taskBodyPath)
            if ($taskUrl -notmatch '^https://github.com/Block361/AudioTools/issues/(\d+)$') {
                throw 'Issue creation returned an unknown result; inspect GitHub before trying again.'
            }
            $taskNumber = [int]$Matches[1]
        }
        $taskIssue | Add-Member -NotePropertyName number -NotePropertyValue $taskNumber -Force
        $taskIssue | Add-Member -NotePropertyName url -NotePropertyValue $taskUrl -Force
        Save-Manifest
    }
    foreach ($taskIssue in $taskManifest.issues) {
        $taskBody = [IO.File]::ReadAllText((Join-Path $taskDocs $taskIssue.file))
        foreach ($taskOther in $taskManifest.issues) {
            $taskBody = $taskBody.Replace('(./' + $taskOther.file + ')', '(' + $taskOther.url + ')')
        }
        if ($taskBody -match '\]\(\./') { throw "Unresolved relative issue link: $($taskIssue.title)" }
        $taskBodyPath = Join-Path $taskTemporary ($taskIssue.key + '.md')
        [IO.File]::WriteAllText($taskBodyPath, $taskBody, $taskUtf8)
        Invoke-Gh @('issue','edit',"$($taskIssue.number)",'--repo',$taskRepository,'--title',$taskIssue.title,'--add-label',$taskLabel,'--body-file',$taskBodyPath) | Out-Null
        $taskRemote = Invoke-Gh @('issue','view',"$($taskIssue.number)",'--repo',$taskRepository,'--json','body,title,labels,url') | ConvertFrom-Json
        if ($taskRemote.title -ne $taskIssue.title -or $taskLabel -notin $taskRemote.labels.name -or
            $taskRemote.body.Replace("`r`n","`n").TrimEnd() -cne $taskBody.Replace("`r`n","`n").TrimEnd()) {
            throw "Remote documentation verification failed: $($taskIssue.title)"
        }
        foreach ($taskOther in $taskManifest.issues) {
            if ($taskOther.key -ne $taskIssue.key -and -not $taskRemote.body.Contains($taskOther.url)) {
                throw "Missing cross-link from $($taskIssue.title) to $($taskOther.title)"
            }
        }
        Write-Output "$($taskIssue.title): $($taskIssue.url)"
    }
    $taskManifest.publicationStatus = 'published'
    Save-Manifest
    Write-Output "Verified $($taskManifest.issues.Count) documentation issues, labels and all cross-links."
} finally {
    $env:GH_TOKEN = $taskPreviousToken
    $env:GIT_TERMINAL_PROMPT = $taskPreviousPrompt
    $env:GCM_INTERACTIVE = $taskPreviousInteractive
    # The small UTF-8 issue bodies are retained in this task-specific temporary directory for inspection.
}
