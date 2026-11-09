if exists('b:did_ftplugin')
    finish
endif
let b:did_ftplugin = 1

setlocal commentstring=//\ %s
setlocal comments=s1:/*,mb:*,ex:*/,://
setlocal suffixesadd=.fl
setlocal makeprg=longbow\ run
let &l:errorformat = join([
            \ '%f:FLECHA-%t%n:%l:%c: %m',
            \ '%E%\s%#test: %f:%l:%c',
            \ ], ',')

let b:undo_ftplugin =
            \ 'setlocal commentstring< comments< suffixesadd< makeprg< errorformat<'
