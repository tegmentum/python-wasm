import { initialize, runPython } from './python-runner.js'
import { examples } from './examples.js'

const editor = document.getElementById('editor') as HTMLTextAreaElement
const output = document.getElementById('output') as HTMLPreElement
const runBtn = document.getElementById('run-btn') as HTMLButtonElement
const exampleSelect = document.getElementById('example-select') as HTMLSelectElement
const status = document.getElementById('status') as HTMLSpanElement

let ready = false

// Populate example dropdown
examples.forEach((example, i) => {
  const option = document.createElement('option')
  option.value = String(i)
  option.textContent = example.name
  exampleSelect.appendChild(option)
})

// Set initial code
editor.value = examples[0].code

exampleSelect.addEventListener('change', () => {
  const idx = parseInt(exampleSelect.value, 10)
  if (idx >= 0 && idx < examples.length) {
    editor.value = examples[idx].code
  }
})

runBtn.addEventListener('click', async () => {
  if (!ready) return

  const code = editor.value
  if (!code.trim()) {
    output.textContent = '(no code to run)'
    return
  }

  runBtn.disabled = true
  status.textContent = 'Running...'
  status.className = 'status running'
  output.textContent = ''

  try {
    const result = await runPython(code)

    output.textContent = ''

    if (result.stdout) {
      const stdoutEl = document.createElement('span')
      stdoutEl.textContent = result.stdout
      output.appendChild(stdoutEl)
    }

    if (result.stderr) {
      const stderrEl = document.createElement('span')
      stderrEl.className = 'stderr'
      stderrEl.textContent = result.stderr
      output.appendChild(stderrEl)
    }

    if (!result.stdout && !result.stderr) {
      output.textContent = `(exit code ${result.exitCode})`
    }

    status.textContent = `Done (exit ${result.exitCode})`
    status.className = result.exitCode === 0 ? 'status ready' : 'status error'
  } catch (e: unknown) {
    const message = e instanceof Error ? e.message : String(e)
    output.textContent = `Error: ${message}`
    status.textContent = 'Error'
    status.className = 'status error'
  } finally {
    runBtn.disabled = false
  }
})

// Ctrl/Cmd+Enter to run
editor.addEventListener('keydown', (e: KeyboardEvent) => {
  if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
    e.preventDefault()
    runBtn.click()
  }
})

// Initialize
async function init() {
  status.textContent = 'Loading...'
  status.className = 'status loading'
  runBtn.disabled = true

  try {
    await initialize((msg) => {
      status.textContent = msg
    })
    ready = true
    runBtn.disabled = false
    status.textContent = 'Ready'
    status.className = 'status ready'
  } catch (e: unknown) {
    const message = e instanceof Error ? e.message : String(e)
    status.textContent = `Failed: ${message}`
    status.className = 'status error'
    console.error('Initialization failed:', e)
  }
}

init()
