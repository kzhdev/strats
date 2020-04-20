if !has('python')
  echo "Error: Required vim compiled with +python"
  finish
endif

" Reload Guard
"
if (exists('g:did_logviewer') && g:did_logviewer)
    finish
endif

let s:plugin_root_dir = fnamemodify(resolve(expand('<sfile>:p')), ':h')

" Python bootstrap
"
python << EOF

import sys
from os.path import normpath, join
import vim
plugin_root_dir = vim.eval('s:plugin_root_dir')
python_root_dir = normpath(join(plugin_root_dir, "..", "python"))
sys.path.insert(0, python_root_dir)
import logViewer
viewer = logViewer.LogViewer()

EOF

" functions and commands
"
function! Log()
    python viewer.loadLog()
endfunction
command! -nargs=0 Log call Log()
nnoremap <silent> <C-l> :call Log()<CR>

function! ShowTemplate()
    python viewer.showTemplate()
endfunction
command! -nargs=0 ShowTemplate call ShowTemplate()
nnoremap <silent> <C-t> :call ShowTemplate()<CR>

function! ShowContracts()
    python viewer.showContracts()
endfunction
command! -nargs=0 ShowContracts call ShowContracts()

function! Close()
    python viewer.close()
endfunction
command! -nargs=0 Close call Close()
nnoremap <silent> <C-h> :call Close()<CR>

function! LoadMore()
    python viewer.loadMore()
endfunction

function! OnBufDelete()
    python viewer.onBufDelete()
endfunction

function! Filter()
    python viewer.filterContract()
endfunction
command! -nargs=0 Filter call Filter()
nnoremap <silent> <C-f> :call Filter()<CR><CR>

augroup vgroup
    autocmd!
    autocmd BufDelete * :call OnBufDelete()
    autocmd CursorMoved * :call LoadMore()
augroup end
cnoremap <expr> <CR> getcmdtype() == '/' ? '<CR>call LoadMore()' : '<CR>'

function! JumpTo()
    python viewer.jumpTo()
endfunction
command! -nargs=0 JumpTo call JumpTo()
nnoremap <silent> <C-j> :call JumpTo()<CR><CR>

function! OpenPreviousLog()
    python viewer.openPreviousLogFile()
endfunction
command! -nargs=0 OpenPreviousLog : call OpenPreviousLog()
