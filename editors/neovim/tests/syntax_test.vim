filetype plugin indent on
syntax enable

execute 'edit ' . fnameescape(
            \ expand('<sfile>:p:h') . '/fixtures/highlighting.fl')

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

call assert_equal('flecha', &filetype)
call assert_equal('flecha', get(b:, 'current_syntax', ''))
call assert_equal('// %s', &l:commentstring)
call assert_equal('flechaType', s:HighlightAt('stringv', 0))
call assert_equal('flechaFunction', s:HighlightAt('Echo', 0))
call assert_equal('flechaFunction', s:HighlightAt('generic_identity<T>', 0))
call assert_equal('flechaFunction', s:HighlightAt('generic_identity<i32>', 0))
call assert_equal('flechaLineComment', s:HighlightAt('// NOTE', 0))
call assert_equal('flechaNumber', s:HighlightAt('42', 0))
call assert_equal('flechaFloat', s:HighlightAt('4.2', 0))
call assert_equal('flechaCharacter', s:HighlightAt("'\\n'", 0))
call assert_equal('flechaString', s:HighlightAt('"flecha\n"', 0))
call assert_equal('flechaRepeat', s:HighlightAt('while', 0))
call assert_equal('flechaBoolean', s:HighlightAt('true', 0))
call assert_equal('flechaWordOperator', s:HighlightAt('and', 0))
call assert_equal('flechaConditional', s:HighlightAt('also', 0))
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
call assert_equal('flechaFuncKeyword', s:HighlightAt('func Mapper<T>', 0))
call assert_equal('flechaModifier', s:HighlightAt('once func Action', 0))
call assert_equal('flechaType', s:HighlightAt('Mapper<i32>', 0))
call assert_equal('flechaModifier', s:HighlightAt('copy factor', 0))
call assert_equal('flechaOperator', s:HighlightAt('[&factor', 1))
call assert_equal('flechaModifier', s:HighlightAt('&const limit', 1))
call assert_equal('flechaModifier', s:HighlightAt('lend pointer', 0))
call assert_equal('flechaModifier', s:HighlightAt('own owner', 0))
call assert_equal('flechaModifier', s:HighlightAt('own copied', 0))
call assert_equal('#569cd6', tolower(synIDattr(hlID('flechaFuncKeyword'), 'fg#', 'gui')))
call assert_equal('68', synIDattr(hlID('flechaFuncKeyword'), 'fg', 'cterm'))
call assert_equal('flechaTest', s:HighlightAt('test gold_keyword', 0))
call assert_equal('#d4af37', tolower(synIDattr(hlID('flechaTest'), 'fg#', 'gui')))
call assert_equal('178', synIDattr(hlID('flechaTest'), 'fg', 'cterm'))

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
call assert_equal('flechaFunction', s:HighlightAt('values.len()', 7))

call assert_equal('flechaRepeat', s:HighlightAt('for (i', 0))
call assert_equal('flechaRepeat', s:HighlightAt('range values', 0))
call assert_equal('flechaRepeat', s:HighlightAt('loop usize', 0))
call assert_equal('flechaOperator', s:HighlightAt('value++', 5))

call assert_equal('flechaModifier', s:HighlightAt('[]const i32', 2))
call assert_equal('flechaType', s:HighlightAt('[]const i32', 8))
call assert_equal('flechaNumber', s:HighlightAt('values[1:3]', 7))
call assert_equal('flechaFunction', s:HighlightAt('readonly.front()', 9))
call assert_equal('flechaFunction', s:HighlightAt('fixed.back()', 6))

call assert_equal('flechaType', s:HighlightAt('(i32, bool)', 1))
call assert_equal('flechaFunction', s:HighlightAt('TupleResult()', 0))
call assert_equal('flechaType', s:HighlightAt('bool ready', 0))

call assert_equal('flechaType', s:HighlightAt('i32 quotient', 0))

call assert_equal('flechaType', s:HighlightAt('error MathError', 0))
call assert_equal('flechaType', s:HighlightAt('error genericError', 0))

call assert_equal('flechaStatement', s:HighlightAt('structure', 0))
call assert_equal('flechaStatement', s:HighlightAt('enum State', 0))
call assert_equal('flechaStatement', s:HighlightAt('build', 0))
call assert_equal('flechaModifier', s:HighlightAt('private', 0))
call assert_equal('flechaModifier', s:HighlightAt('override', 0))
call assert_equal('flechaOperator', s:HighlightAt('::', 0))
call assert_equal('flechaOperator', s:HighlightAt('->', 0))

call assert_equal('flechaAllocation', s:HighlightAt('new<u8>', 0))
call assert_equal('flechaAllocation', s:HighlightAt('new<[]i32>', 0))
call assert_equal('flechaAllocation', s:HighlightAt('take(self->storage)', 0))
call assert_equal('flechaSelf', s:HighlightAt('take(self->storage)', 5))
call assert_equal('#5f87af', tolower(synIDattr(hlID('flechaSelf'), 'fg#', 'gui')))
call assert_equal('67', synIDattr(hlID('flechaSelf'), 'fg', 'cterm'))
call assert_equal('flechaType', s:HighlightAt('AllocationError', 0))

call assert_equal('flechaTypeSetKeyword', s:HighlightAt('typeset scalar_numbers', 0))
call assert_equal('flechaTypeSetName', s:HighlightAt('scalar_numbers', 0))
call assert_equal('flechaBuiltinTypeSet', s:HighlightAt('signed_integer', 0))
call assert_equal('flechaBuiltinTypeSet', s:HighlightAt('unsigned_integer', 0))
call assert_equal('flechaBuiltinTypeSet', s:HighlightAt('floating_point', 0))
call assert_equal('flechaBuiltinTypeSet', s:HighlightAt('T: numeric', 3))
call assert_equal('flechaIsOfMethod', s:HighlightAt('.isof<integer>', 0))
call assert_equal('flechaBuiltinTypeSet', s:HighlightAt('.isof<integer>', 6))
call assert_equal('#87cefa', tolower(synIDattr(hlID('flechaBuiltinTypeSet'), 'fg#', 'gui')))
call assert_equal('117', synIDattr(hlID('flechaBuiltinTypeSet'), 'fg', 'cterm'))
call assert_equal('flechaIntegerLimit', s:HighlightAt('i32.min_value', 4))
call assert_equal('flechaIntegerLimit', s:HighlightAt('usize.max_value', 6))
call assert_equal('flechaIntegerLimit', s:HighlightAt('T.max_value', 2))
call assert_notequal('flechaIntegerLimit', s:HighlightAt('min_value :=', 0))
call assert_notequal('flechaIntegerLimit', s:HighlightAt('max_value :=', 0))
call assert_equal('#c49a00', tolower(synIDattr(hlID('flechaIntegerLimit'), 'fg#', 'gui')))
call assert_equal('178', synIDattr(hlID('flechaIntegerLimit'), 'fg', 'cterm'))
call assert_equal('flechaNamespace', s:HighlightAt('integer::answer', 0))

call assert_equal('flechaModifier', s:HighlightAt('own *i32', 0))
call assert_equal('flechaModifier', s:HighlightAt('lend *const i32', 0))

call assert_equal('flechaImports', s:HighlightAt('@imports ::', 0))
call assert_equal('flechaImports', s:HighlightAt('@imports ::', 7))
call assert_notequal('flechaImports', s:HighlightAt('@imports_extra', 0))
call assert_equal('flechaLineComment', s:HighlightAt('@imports comment', 0))
call assert_equal('flechaString', s:HighlightAt('@imports string', 0))
call assert_equal('#87cefa', tolower(synIDattr(hlID('flechaImports'), 'fg#', 'gui')))
call assert_equal('117', synIDattr(hlID('flechaImports'), 'fg', 'cterm'))

call assert_equal('flechaStatement', s:HighlightAt('module integer', 0))
call assert_equal('flechaModuleName', s:HighlightAt('module integer', 7))
call assert_equal('flechaModifier', s:HighlightAt('export i32', 0))
call assert_equal('flechaModifier', s:HighlightAt('quiver i32', 0))

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

if !empty(v:errors)
    for error in v:errors
        echomsg error
    endfor
    cquit 1
endif

quit
