if exists('g:loaded_flecha_longbow')
    finish
endif
let g:loaded_flecha_longbow = 1

let s:errorformat = '%f:FLECHA-%t%n:%l:%c: %m'

function! s:LongbowRun() abort
    if &buftype ==# '' && !empty(expand('%:p'))
        let start = expand('%:p:h')
    else
        let start = getcwd()
    endif
    let manifest = findfile('longbow.toml', start . ';')
    if empty(manifest)
        echoerr 'Longbow: no longbow.toml found from ' . start
        return
    endif

    if &buftype ==# '' && !empty(expand('%:p'))
        silent update
    endif

    let saved_makeprg = &l:makeprg
    let saved_errorformat = &l:errorformat
    try
        let &l:makeprg = get(b:, 'flecha_longbow_makeprg', 'longbow run')
        let &l:errorformat = s:errorformat
        execute 'silent make! ' . fnameescape(fnamemodify(manifest, ':p:h'))
    finally
        let &l:makeprg = saved_makeprg
        let &l:errorformat = saved_errorformat
    endtry

    botright copen
    nnoremap <buffer> <silent> <leader>fe :call <SID>LongbowSelectedError()<CR>
    nnoremap <buffer> <silent> <CR> :call <SID>LongbowSelectedError()<CR>

    let errors = len(filter(copy(getqflist()), 'v:val.valid'))
    redraw
    echomsg printf('Longbow: %d error%s', errors, errors == 1 ? '' : 's')
endfunction

function! s:LongbowSelectedError() abort
    let quickfix_index = line('.')
    let items = getqflist()
    if quickfix_index < 1 || quickfix_index > len(items)
        echoerr 'Longbow: no result is selected'
        return
    endif

    let item = items[quickfix_index - 1]
    if !item.valid
        echoerr 'Longbow: the selected result is not a source location'
        return
    endif

    execute quickfix_index . 'cc'
    call cursor(item.lnum, max([1, item.col]))
endfunction

command! LongbowRun call <SID>LongbowRun()

nnoremap <silent> <Plug>(FlechaLongbowRun) :LongbowRun<CR>
if !hasmapto('<Plug>(FlechaLongbowRun)', 'n')
            \ && empty(maparg('<leader>rr', 'n'))
    nmap <leader>rr <Plug>(FlechaLongbowRun)
endif
