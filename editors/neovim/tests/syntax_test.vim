filetype plugin indent on
syntax enable
let g:flecha_lsp_autostart = 0
runtime plugin/flecha_lsp.lua

" The run command and mapping must exist before any Flecha file is opened.
call assert_equal('', expand('%:p'))
call assert_equal(2, exists(':LongbowRun'))
call assert_equal(2, exists(':LongbowTest'))
call assert_equal(2, exists(':FlechaLspInfo'))
call assert_notequal('', maparg('<leader>rr', 'n'))
call assert_notequal('', maparg('<leader>rt', 'n'))
call assert_equal('', maparg('<leader>lr', 'n'))
call assert_equal('', maparg('<leader>fr', 'n'))

" Exercise running from an empty Neovim at a project root, followed by a jump
" into a previously unopened Flecha buffer. An intentionally out-of-range
" end-of-line column must remain on the diagnostic's recorded source line.
let s:test_directory = expand('<sfile>:p:h')
let s:fixture_directory = s:test_directory . '/fixtures'
let s:jump_target = s:fixture_directory . '/jump_target.fl'
let s:diagnostic = s:jump_target
            \ . ':FLECHA-S0002:1:999: error at the end of the line'
let s:original_directory = getcwd()
execute 'lcd ' . fnameescape(s:fixture_directory)
let b:flecha_longbow_makeprg = 'echo ' . shellescape(s:diagnostic)
LongbowRun
call assert_equal('quickfix', &buftype)
call assert_notequal('', maparg('<leader>fe', 'n'))
call assert_notequal('', maparg('<CR>', 'n'))
call cursor(1, 1)
call feedkeys("\\fe", 'xt')
call assert_equal(1, line('.'))
call assert_equal(fnamemodify(s:jump_target, ':p'), expand('%:p'))
call assert_equal('flecha', &filetype)
cclose
execute 'lcd ' . fnameescape(s:original_directory)

execute 'edit ' . fnameescape(
            \ s:fixture_directory . '/highlighting.fl')

function! s:HighlightAt(text, offset) abort
    call cursor(1, 1)
    let position = searchpos('\V' . escape(a:text, '\'), 'W')
    call assert_notequal([0, 0], position, 'missing fixture text: ' . a:text)
    if position ==# [0, 0]
        return ''
    endif

    let column = position[1] + a:offset
    return synIDattr(synID(position[0], column, 1), 'name')
endfunction

function! s:HighlightLink(group, semantic_group, fallback_group) abort
    let output = execute('silent highlight ' . a:group)
    let target = hlexists(a:semantic_group)
                \ ? a:semantic_group
                \ : a:fallback_group
    call assert_true(stridx(output, 'links to ' . target) >= 0, a:group)
endfunction

call assert_equal('flecha', &filetype)
call assert_equal('flecha', get(b:, 'current_syntax', ''))
call assert_equal('// %s', &l:commentstring)
call assert_equal('longbow run', &l:makeprg)
call assert_equal(
            \ '%f:FLECHA-%t%n:%l:%c: %m,%E%\s%#test: %f:%l:%c',
            \ &l:errorformat)
call assert_equal(2, exists(':LongbowRun'))
call assert_equal(2, exists(':LongbowTest'))
call assert_notequal('', maparg('<leader>rr', 'n'))
call assert_notequal('', maparg('<leader>rt', 'n'))

let b:flecha_longbow_test_makeprg = 'echo ' . shellescape(s:diagnostic)
LongbowTest
call assert_equal('quickfix', &buftype)
let s:longbow_test_errors = filter(copy(getqflist()), 'v:val.valid')
call assert_equal(1, len(s:longbow_test_errors))
call assert_match('--verbose$', s:longbow_test_errors[0].text)
cclose

" A failed test reports its declaration after non-location output. From that
" first quickfix line, <leader>fe must find and open the valid test location.
call setqflist([], 'r', {
            \ 'lines': [
            \     'framing_test.fl',
            \     '  writes_complete_message................................fail',
            \     '    test: ' . s:jump_target . ':1:1',
            \     'FAIL [0/1] tests passed, but [1/1] failed',
            \ ],
            \ 'efm': &l:errorformat,
            \ })
copen
call cursor(1, 1)
call feedkeys("\\fe", 'xt')
call assert_equal(fnamemodify(s:jump_target, ':p'), expand('%:p'))
call assert_equal(1, line('.'))
cclose
execute 'edit ' . fnameescape(
            \ s:fixture_directory . '/highlighting.fl')

call setqflist([], 'r', {
            \ 'lines': [
            \     expand('%:p') . ':FLECHA-S0002:17:9: unknown name ''end''',
            \     expand('%:p') . ':FLECHA-S0008:21:4: bad index',
            \ ],
            \ 'efm': &l:errorformat,
            \ })
let s:longbow_errors = getqflist()
call assert_equal(2, len(s:longbow_errors))
call assert_equal(17, s:longbow_errors[0].lnum)
call assert_equal(9, s:longbow_errors[0].col)
call assert_equal('S', s:longbow_errors[0].type)
call assert_equal(2, s:longbow_errors[0].nr)

call assert_equal('flechaType', s:HighlightAt('stringv', 0))
call assert_equal('flechaFunction', s:HighlightAt('Echo', 0))
call assert_equal('flechaParameter', s:HighlightAt('stringv value', 8))
call assert_equal('flechaVariable', s:HighlightAt('count := 42', 0))
call assert_equal('flechaNamedConstant', s:HighlightAt('METHOD = "GET"', 0))
call assert_equal('flechaEnumConstant', s:HighlightAt('MathError.Overflow', 10))
call s:HighlightLink('flechaType', '@type', 'Type')
call s:HighlightLink('flechaFunction', '@function', 'Function')
call s:HighlightLink('flechaMethod', '@function.method', 'Function')
call s:HighlightLink('flechaParameter', '@variable.parameter', 'Identifier')
call s:HighlightLink('flechaVariable', '@variable', 'Identifier')
call s:HighlightLink('flechaNamedConstant', '@constant', 'Constant')
call s:HighlightLink('flechaEnumConstant', '@constant', 'Constant')
call assert_equal('flechaFunction', s:HighlightAt('generic_identity<T>', 0))
call assert_equal('flechaFunction', s:HighlightAt('generic_identity<i32>', 0))
call assert_equal('flechaFunction', s:HighlightAt('explode!()', 0))
call assert_equal('flechaCrashableBang', s:HighlightAt('explode!()', 7))
call assert_equal('flechaFunction', s:HighlightAt('generic_explode<T>!(', 0))
call assert_equal('flechaCrashableBang', s:HighlightAt('generic_explode<T>!(', 18))
call assert_equal('flechaMethod', s:HighlightAt('CrashableWorker::method!(', 17))
call assert_equal('flechaCrashableBang', s:HighlightAt('CrashableWorker::method!(', 23))
call assert_equal('flechaCrashBuiltin', s:HighlightAt('crash("boom")', 0))
call s:HighlightLink('flechaCrashBuiltin', '@function.builtin', 'Function')
call assert_equal('flechaFunction', s:HighlightAt('crash_handler()', 0))
call s:HighlightLink('flechaCrashableBang', '@operator', 'Operator')
call assert_equal('flechaOperator', s:HighlightAt('!false', 0))
call assert_equal('flechaLineComment', s:HighlightAt('// NOTE', 0))
call assert_equal('flechaNumber', s:HighlightAt('42', 0))
call assert_equal('flechaNumber', s:HighlightAt('0x6a09e667', 0))
call assert_equal('flechaNumber', s:HighlightAt('0x6a09e667', 9))
call assert_equal('flechaFloat', s:HighlightAt('4.2', 0))
call assert_equal('flechaCharacter', s:HighlightAt("'\\n'", 0))
call assert_equal('flechaString', s:HighlightAt('"flecha\n"', 0))
call assert_equal('flechaRepeat', s:HighlightAt('while', 0))
call assert_equal('flechaBoolean', s:HighlightAt('true', 0))
call assert_equal('flechaBoolean', s:HighlightAt('false', 0))
call assert_equal('flechaWordOperator', s:HighlightAt('and', 0))
call assert_equal('flechaConditional', s:HighlightAt('also', 0))
call assert_equal('flechaConditional', s:HighlightAt('else {', 0))
call assert_equal('flechaConditional', s:HighlightAt('if count', 0))
call assert_equal('flechaConditional', s:HighlightAt('match state', 0))
call assert_equal('flechaOperator', s:HighlightAt('=> true', 0))
call assert_equal('flechaOperator', s:HighlightAt(':=', 0))
call assert_equal('flechaOperator', s:HighlightAt('&&', 0))
call assert_equal('flechaOperator', s:HighlightAt('||', 0))
call assert_equal('flechaOperator', s:HighlightAt('~(', 0))
call assert_equal('flechaOperator', s:HighlightAt(' & ', 1))
call assert_equal('flechaOperator', s:HighlightAt(' | ', 1))
call assert_equal('flechaOperator', s:HighlightAt('<<', 0))
call assert_equal('flechaOperator', s:HighlightAt('>>', 0))
call assert_equal('flechaCastMethod', s:HighlightAt('.cast', 0))
call assert_equal('flechaCast', s:HighlightAt(' as ', 1))
call assert_equal('flechaJump', s:HighlightAt('jumpto', 0))
call assert_equal('flechaJumpTarget', s:HighlightAt('final_position;', 0))
call assert_equal('flechaLabel', s:HighlightAt('#final_position', 0))
call assert_equal('flechaStatement', s:HighlightAt('proceed', 0))
call assert_equal('flechaStatement', s:HighlightAt('break', 0))
call assert_equal('flechaStatement', s:HighlightAt('return', 0))
call assert_equal('flechaStatement', s:HighlightAt('defer', 0))
call assert_equal('flechaStatement', s:HighlightAt('yield none', 0))
call assert_equal('flechaConstant', s:HighlightAt('yield none', 6))
call assert_equal('flechaFuncKeyword', s:HighlightAt('func Mapper<T>', 0))
call assert_equal('flechaModifier', s:HighlightAt('once func Action', 0))
call assert_equal('flechaType', s:HighlightAt('Mapper<i32>', 0))
call assert_equal('flechaModifier', s:HighlightAt('copy factor', 0))
call assert_equal('flechaOperator', s:HighlightAt('[&factor', 1))
call assert_equal('flechaModifier', s:HighlightAt('&const limit', 1))
call assert_equal('flechaModifier', s:HighlightAt('lend pointer', 0))
call assert_equal('flechaModifier', s:HighlightAt('own owner', 0))
call assert_equal('flechaModifier', s:HighlightAt('own copied', 0))
call s:HighlightLink('flechaFuncKeyword', '@keyword.function', 'Keyword')
call assert_equal('flechaTest', s:HighlightAt('test gold_keyword', 0))
call s:HighlightLink('flechaTest', '@keyword', 'Keyword')

call assert_equal('flechaModifier', s:HighlightAt('constptr', 0))
call assert_equal('flechaModifier', s:HighlightAt('constref', 0))
call assert_equal('flechaModifier', s:HighlightAt('&const', 1))
call assert_equal('flechaOperator', s:HighlightAt('&const', 0))
call assert_notequal('flechaModifier', s:HighlightAt('constval', 0))

call assert_equal('flechaRawAddress', s:HighlightAt('&raw', 1))
call assert_notequal('flechaRawAddress', s:HighlightAt('raw :=', 0))

call assert_equal('flechaNumber', s:HighlightAt('[5]i32', 1))
call assert_equal('flechaType', s:HighlightAt('[5]i32', 3))
call assert_equal('flechaNumber', s:HighlightAt('[7; 5]', 4))
call assert_equal('flechaOperator', s:HighlightAt('&values[1]', 0))
call assert_equal('flechaMethod', s:HighlightAt('values.len()', 7))

call assert_equal('flechaRepeat', s:HighlightAt('for (i', 0))
call assert_equal('flechaRepeat', s:HighlightAt('range values', 0))
call assert_equal('flechaRepeat', s:HighlightAt('loop usize', 0))
call assert_equal('flechaOperator', s:HighlightAt('value++', 5))

call assert_equal('flechaModifier', s:HighlightAt('[]const i32', 2))
call assert_equal('flechaType', s:HighlightAt('[]const i32', 8))
call assert_equal('flechaNumber', s:HighlightAt('values[1:3]', 7))
call assert_equal('flechaMethod', s:HighlightAt('readonly.front()', 9))
call assert_equal('flechaMethod', s:HighlightAt('fixed.back()', 6))
call assert_equal('flechaFunction', s:HighlightAt('cast<T>()', 0))

call assert_equal('flechaType', s:HighlightAt('(i32, bool)', 1))
call assert_equal('flechaFunction', s:HighlightAt('TupleResult()', 0))
call assert_equal('flechaType', s:HighlightAt('bool ready', 0))

call assert_equal('flechaType', s:HighlightAt('i32 quotient', 0))

call assert_equal('flechaType', s:HighlightAt('error MathError', 0))
call assert_equal('flechaType', s:HighlightAt('error genericError', 0))

call assert_equal('flechaStatement', s:HighlightAt('structure', 0))
call assert_equal('flechaStatement', s:HighlightAt('enum State', 0))
call assert_equal('flechaStatement', s:HighlightAt('build', 0))
call assert_equal('flechaMethod', s:HighlightAt('Server::run()', 8))
call assert_equal('flechaModifier', s:HighlightAt('private', 0))
call assert_equal('flechaModifier', s:HighlightAt('override', 0))
call assert_equal('flechaOperator', s:HighlightAt('::', 0))
call assert_equal('flechaOperator', s:HighlightAt('->', 0))

call assert_equal('flechaAllocation', s:HighlightAt('new<u8>', 0))
call assert_equal('flechaAllocation', s:HighlightAt('new<[]i32>', 0))
call assert_equal('flechaAllocation', s:HighlightAt('take(self->storage)', 0))
call assert_equal('flechaSelf', s:HighlightAt('take(self->storage)', 5))
call assert_equal('flechaSelf', s:HighlightAt('<- ...args', 6))
call assert_notequal('flechaSelf', s:HighlightAt('args := 0', 0))
call s:HighlightLink('flechaSelf', '@variable.builtin', 'Special')
call assert_equal('flechaType', s:HighlightAt('AllocationError', 0))

call assert_equal('flechaTypeSetKeyword', s:HighlightAt('typeset scalar_numbers', 0))
call assert_equal('flechaTypeSetName', s:HighlightAt('scalar_numbers', 0))
call assert_equal('flechaBuiltinTypeSet', s:HighlightAt('signed_integer', 0))
call assert_equal('flechaBuiltinTypeSet', s:HighlightAt('unsigned_integer', 0))
call assert_equal('flechaBuiltinTypeSet', s:HighlightAt('floating_point', 0))
call assert_equal('flechaBuiltinTypeSet', s:HighlightAt('T: numeric', 3))
call assert_equal('flechaIsOfMethod', s:HighlightAt('.isof<integer>', 0))
call assert_equal('flechaBuiltinTypeSet', s:HighlightAt('.isof<integer>', 6))
call s:HighlightLink('flechaBuiltinTypeSet', '@type.builtin', 'Type')
call assert_equal('flechaIntegerLimit', s:HighlightAt('i32.min_value', 4))
call assert_equal('flechaIntegerLimit', s:HighlightAt('usize.max_value', 6))
call assert_equal('flechaIntegerLimit', s:HighlightAt('T.max_value', 2))
call assert_notequal('flechaIntegerLimit', s:HighlightAt('min_value :=', 0))
call assert_notequal('flechaIntegerLimit', s:HighlightAt('max_value :=', 0))
call s:HighlightLink('flechaIntegerLimit', '@constant', 'Constant')
call assert_equal('flechaFloatConstant', s:HighlightAt('f32.nan', 4))
call assert_equal('flechaFloatConstant', s:HighlightAt('f64.infinity', 4))
call assert_equal('flechaFloatConstant', s:HighlightAt('f32.negative_infinity', 4))
call assert_equal('flechaFloatConstant', s:HighlightAt('f32.min_positive', 4))
call assert_equal('flechaFloatConstant', s:HighlightAt('T.epsilon', 2))
call s:HighlightLink('flechaFloatConstant', '@constant', 'Constant')
call assert_equal('flechaNamespace', s:HighlightAt('integer::answer', 0))

call assert_equal('flechaModifier', s:HighlightAt('own *i32', 0))
call assert_equal('flechaModifier', s:HighlightAt('lend *const i32', 0))

call assert_equal('flechaImports', s:HighlightAt('@imports ::', 0))
call assert_equal('flechaImports', s:HighlightAt('@imports ::', 7))
call assert_equal('flechaImports', s:HighlightAt('@imports_extra', 0))
call assert_equal('flechaLineComment', s:HighlightAt('@imports comment', 0))
call assert_equal('flechaString', s:HighlightAt('@imports string', 0))
call assert_equal('flechaImports', s:HighlightAt('@json(structure)', 0))
call assert_equal('flechaImports', s:HighlightAt('@json(structure)', 4))
call assert_equal('flechaImports', s:HighlightAt('@json("value")', 0))
call assert_equal('flechaImports', s:HighlightAt('@annotations::json("test")', 2))
call assert_equal('flechaImports', s:HighlightAt('@annotations::json("test")', 15))
call assert_equal('flechaLineComment', s:HighlightAt('@json comment', 0))
call assert_equal('flechaString', s:HighlightAt('@json string', 0))
call s:HighlightLink('flechaImports', '@keyword.import', 'Include')

call assert_equal('flechaStatement', s:HighlightAt('module integer', 0))
call assert_equal('flechaStatement', s:HighlightAt('decoration json', 0))
call assert_equal('flechaModuleName', s:HighlightAt('module integer', 7))
call assert_equal('flechaModifier', s:HighlightAt('export i32', 0))
call assert_equal('flechaModifier', s:HighlightAt('quiver i32', 0))
call assert_equal('flechaModifier', s:HighlightAt('foreign c', 0))

" A colorscheme reload must preserve Flecha's links and remain authoritative.
colorscheme default
call s:HighlightLink('flechaType', '@type', 'Type')
call s:HighlightLink('flechaFunction', '@function', 'Function')
call s:HighlightLink('flechaParameter', '@variable.parameter', 'Identifier')
call s:HighlightLink('flechaVariable', '@variable', 'Identifier')
call s:HighlightLink('flechaNamedConstant', '@constant', 'Constant')

" Exercise actual Enter/closing-brace typing, not just the '=' operator.
enew
setfiletype flecha
setlocal shiftwidth=4 expandtab
call assert_true(&l:cindent)
call feedkeys("ii32 test() {\<CR>}\<Esc>", 'xt')
call assert_equal(['i32 test() {', '}'], getline(1, '$'))

silent %delete _
call feedkeys("ii32 test() {\<CR>if ready {\<CR>return 1;\<CR>}\<CR>}\<Esc>", 'xt')
call assert_equal(['i32 test() {', '    if ready {', '        return 1;', '    }', '}'], getline(1, '$'))

silent %delete _
call feedkeys("iexport structure Box<T> {\<CR>i32 value;\<CR>};\<Esc>", 'xt')
call assert_equal(['export structure Box<T> {', '    i32 value;', '};'], getline(1, '$'))

silent %delete _
call feedkeys("ivoid test() {\<CR>value:\<Esc>", 'xt')
call assert_equal('    value:', getline(2))
call feedkeys("A= 42;\<CR>math::run();\<CR>part := values[0:2];\<CR>}\<Esc>", 'xt')
call assert_equal(['void test() {', '    value:= 42;', '    math::run();', '    part := values[0:2];', '}'], getline(1, '$'))
bwipeout!

execute 'edit ' . fnameescape(s:fixture_directory . '/highlighting.fl')
execute 'luafile ' . fnameescape(s:test_directory . '/lsp_test.lua')

if !empty(v:errors)
    for error in v:errors
        echomsg error
    endfor
    cquit 1
endif

quit
