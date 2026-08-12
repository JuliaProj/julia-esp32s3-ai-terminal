param(
    [string]$ComfyUrl = 'http://127.0.0.1:8188',
    [string]$OutputDir = "$PSScriptRoot\..\file\ui_animation\states_v3",
    [string]$StateName = ''
)

$ErrorActionPreference = 'Stop'
[IO.Directory]::CreateDirectory($OutputDir) | Out-Null

$states = [ordered]@{
    's0_1_night_sleep' = 'peacefully asleep, eyes fully closed, relaxed face, very slight downward head tilt, quiet nighttime mood'
    's0_2_day_away' = 'resting while the user is away, eyes gently closed, serene neutral expression, still and unobtrusive'
    's0_3_manual_sleep' = 'sleep mode requested by the user, eyes closed, small reassuring smile, respectful good-night expression'
    's1_1_near_standby' = 'calm attentive standby, warm eye contact, gentle small smile, natural relaxed breathing posture'
    's1_2_far_standby' = 'quietly waiting at a distance, soft unfocused gaze slightly aside, neutral relaxed mouth, low attention'
    's1_3_charging_standby' = 'content and recharging, eyes softly closed, refreshed smile, subtle warm golden charging glow around the chest star badge'
    's2_1_observe' = 'carefully observing the user, focused open eyes, curious attentive expression, slight forward head inclination'
    's2_2_shared_activity' = 'happily accompanying a shared activity, bright engaged eyes, cheerful smile, lively encouraging posture'
    's2_3_bedtime_companion' = 'gentle bedtime companionship, tender sleepy eyes, comforting smile, calm protective expression'
    's3_1_emotion_trigger' = 'noticing the user is upset, empathetic concerned eyes, slightly lowered eyebrows, compassionate supportive expression'
    's3_2_routine_break' = 'gently reminding about an unusual routine, mildly surprised alert eyes, caring questioning expression'
    's3_3_user_call' = 'immediately responding to the user calling, bright alert eyes, welcoming open smile, attentive forward posture'
    's3_4_recovery_probe' = 'carefully checking whether the user feels better, hopeful gentle eyes, small tentative supportive smile'
    's4_1_light_dialog' = 'casual friendly conversation, happy open eyes, natural speaking smile, relaxed sociable expression'
    's4_2_deep_talk' = 'deep sincere conversation, steady empathetic eye contact, thoughtful serious but warm expression'
    's4_3_multi_turn' = 'actively engaged in a continuing conversation, animated attentive eyes, subtle speaking expression, lively responsive posture'
    's4_4_interrupt_handle' = 'politely handling an interruption, briefly surprised eyes, composed patient expression, mouth gently paused'
    's5_1_user_reject' = 'accepting the user rejection without pressure, slightly saddened gentle eyes, restrained understanding smile, respectful expression'
    's5_2_user_perfunctory' = 'sensing a perfunctory response, mildly disappointed but considerate eyes, quiet neutral mouth, giving the user space'
    's5_3_user_left' = 'watching the user leave, soft wistful gaze slightly aside, calm patient expression, quietly waiting'
}

$basePrompt = @'
Use case: identity-preserve. Asset type: 360x360 embedded companion robot character portrait.
Create a polished clean Japanese anime bust portrait of exactly the same Julia character as the reference image. Preserve identity and design exactly: short ivory-white bob hair with pale aqua tips; teal five-point star hair clip with gold outline on her left side; large warm amber-gold eyes; fair skin and soft blush; natural medium-width neck and balanced shoulders; charcoal-trimmed gray-green high collar; pale aqua pleated center insert; ivory cardigan with gray-green piping, one round gold button, and teal-gold star chest badge. Front-facing centered head-and-shoulders framing, same scale and proportions as the reference. Soft neutral off-white background, bright neutral lighting, delicate clean linework, crisp detailed face and clothing, readable at small LCD size.
State expression: {STATE}.
Constraints: change only facial expression, gaze, subtle head angle, and minimal state lighting. Keep face, hairstyle, hair length, hair colors, star hair clip, neck proportions, clothing construction, chest badge, framing, and background consistent across every state.
Avoid: different character, missing hair clip, missing chest badge, hoodie, plain white clothing, blue color cast, long hair, dark hair, exaggerated pose, hands covering face, text, symbols, captions, border, watermark, cropped head.
'@

$workflowTemplate = Get-Content "$PSScriptRoot\julia_pulid_prompt.json" -Raw | ConvertFrom-Json
$sourceV3 = Resolve-Path "$PSScriptRoot\..\file\ui_animation\Julia_v3_final.png"
Copy-Item $sourceV3 (Join-Path $OutputDir 's1_1_near_standby.png') -Force

$index = 0
foreach ($entry in $states.GetEnumerator()) {
    if ($entry.Key -eq 's1_1_near_standby') { continue }
    if ($StateName -and $entry.Key -ne $StateName) { continue }
    $index++
    $workflow = $workflowTemplate | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $workflow.'4'.inputs.image = 'Julia_v3_final.png'
    $workflow.'8'.inputs.weight = 1.0
    $workflow.'9'.inputs.text = ("The required state is the highest priority: " + $entry.Value + ".`n" + $basePrompt.Replace('{STATE}', $entry.Value))
    $workflow.'12'.inputs.noise_seed = 2026072400 + $index * 7919
    $workflow.'14'.inputs.steps = 24
    $workflow.'14'.inputs.denoise = 0.42
    $workflow.'18'.inputs.filename_prefix = "Julia/v3/$($entry.Key)"
    $workflow | Add-Member -NotePropertyName '19' -NotePropertyValue ([pscustomobject]@{
        class_type = 'ImageScale'
        inputs = [pscustomobject]@{ image = @('4', 0); upscale_method = 'lanczos'; width = 768; height = 768; crop = 'disabled' }
    })
    $workflow | Add-Member -NotePropertyName '20' -NotePropertyValue ([pscustomobject]@{
        class_type = 'VAEEncode'
        inputs = [pscustomobject]@{ pixels = @('19', 0); vae = @('3', 0) }
    })
    $workflow.'16'.inputs.latent_image = @('20', 0)

    $body = @{ prompt = $workflow; client_id = 'julia-state-v3-generator' } | ConvertTo-Json -Depth 30
    $queued = Invoke-RestMethod -Uri "$ComfyUrl/prompt" -Method Post -ContentType 'application/json' -Body $body
    $promptId = $queued.prompt_id
    Write-Host "Queued $($entry.Key): $promptId"

    $result = $null
    while (!$result) {
        Start-Sleep -Seconds 2
        $history = Invoke-RestMethod -Uri "$ComfyUrl/history/$promptId" -Method Get
        $record = $history.PSObject.Properties[$promptId].Value
        if ($record.status.status_str -eq 'error') { throw "ComfyUI failed for $($entry.Key)" }
        if ($record.outputs.'18'.images.Count -gt 0) { $result = $record.outputs.'18'.images[0] }
    }

    $query = 'filename={0}&subfolder={1}&type={2}' -f [Uri]::EscapeDataString($result.filename), [Uri]::EscapeDataString($result.subfolder), [Uri]::EscapeDataString($result.type)
    Invoke-WebRequest -Uri "$ComfyUrl/view?$query" -OutFile (Join-Path $OutputDir "$($entry.Key).png")
    Write-Host "Saved $($entry.Key).png"
}

Write-Host "Generated $($states.Count) V3 state portraits in $OutputDir"
