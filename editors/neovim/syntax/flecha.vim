if exists('b:current_syntax')
    finish
endif

syntax case match

syntax match flechaVariable /\<[a-z_][A-Za-z0-9_]*\>/
syntax match flechaImports /@[A-Za-z_][A-Za-z0-9_]*\%(\s*::\s*[A-Za-z_][A-Za-z0-9_]*\)*\>/

syntax keyword flechaType i8 i16 i32 i64 isize
syntax keyword flechaType u8 u16 u32 u64 usize
syntax keyword flechaType f32 f64 bool char void string stringv error AllocationError
syntax match flechaType /\<[A-Z][A-Za-z0-9_]*\>/
syntax match flechaNamedConstant /\<[A-Z][A-Z0-9_]\+\>/
syntax match flechaEnumConstant /\.\zs[A-Z][A-Za-z0-9_]*\>/
syntax match flechaParameter /\%(\<\%(i8\|i16\|i32\|i64\|isize\|u8\|u16\|u32\|u64\|usize\|f32\|f64\|bool\|char\|void\|string\|stringv\|error\|[A-Z][A-Za-z0-9_]*\)\>\s\+\)\@<=\<[a-z_][A-Za-z0-9_]*\>\ze\s*[,)]/
syntax match flechaBuiltinTypeSet /\<\%(signed_integer\|unsigned_integer\|integer\|floating_point\|numeric\)\>/

syntax keyword flechaModifier unsafe once export quiver extern foreign inline comptime copy own lend const constptr constref volatile private protected override virtual default final
syntax keyword flechaFuncKeyword func
syntax keyword flechaTypeSetKeyword typeset nextgroup=flechaTypeSetName skipwhite
syntax match flechaTypeSetName /\<[A-Za-z_][A-Za-z0-9_]*\>/ contained
syntax keyword flechaTest test
syntax keyword flechaConditional if else also match
syntax keyword flechaRepeat while for loop range
syntax keyword flechaStatement return yield proceed break defer enum structure interface decoration build
syntax keyword flechaStatement module nextgroup=flechaModuleName skipwhite
syntax match flechaNamespaceAttachment /\<under\>\_s\+namespace\>/
syntax match flechaModuleName /\<[A-Za-z_][A-Za-z0-9_]*\>/ contained
syntax keyword flechaJump jumpto nextgroup=flechaJumpTarget skipwhite
syntax keyword flechaCast as
syntax keyword flechaBoolean true false
syntax keyword flechaConstant none null
syntax keyword flechaSelf self
syntax match flechaSelf /\%(<-\s*\.\.\.\s*\)\@<=args\>/
syntax keyword flechaWordOperator and or

syntax match flechaOperator /::\|<-\|->\|=>\|:=\|===\|!==\|==\|!=\|<<\|>>\|<=\|>=\|&&\|||\|[&|~=+*\/%^!<>-]/
syntax match flechaInheritanceOperator /<<\ze\_s*\%([A-Za-z_][A-Za-z0-9_]*\_s*::\_s*\)*[A-Z][A-Za-z0-9_]*\_s*\%([,{]\)/
syntax match flechaCrashableBang /\%([A-Za-z0-9_>]\)\@<=!\ze\s*(/
syntax match flechaRawAddress /\%(&\s*\)\@<=raw\>/
syntax match flechaFunction /\<[A-Za-z_][A-Za-z0-9_]*\>\ze\s*\%(<.\{-}>\s*\)\?!\?\s*(/
syntax match flechaMethod /\%(::\s*\)\@<=[A-Za-z_][A-Za-z0-9_]*\ze\s*\%(<.\{-}>\s*\)\?!\?\s*(/
syntax match flechaMethod /\.\zs[A-Za-z_][A-Za-z0-9_]*\ze\s*\%(<.\{-}>\s*\)\?!\?\s*(/
syntax match flechaCrashBuiltin /\<crash\>\ze\s*(/
syntax match flechaAllocation /\<new\>\ze\s*</
syntax match flechaAllocation /\<take\>\ze\s*(/
syntax match flechaCastMethod /\.cast\>\ze\s*</
syntax match flechaIsOfMethod /\.isof\>\ze\s*</
syntax match flechaFloatConstant /\%(\<\%(f32\|f64\|[A-Z][A-Za-z0-9_]*\)\s*\.\s*\)\@<=\%(nan\|infinity\|negative_infinity\|min_value\|max_value\|min_positive\|epsilon\)\>/
syntax match flechaIntegerLimit /\%(\<\%(i8\|i16\|i32\|i64\|isize\|u8\|u16\|u32\|u64\|usize\|[A-Z][A-Za-z0-9_]*\)\s*\.\s*\)\@<=\%(min_value\|max_value\)\>/
syntax match flechaNamespace /\<[A-Za-z_][A-Za-z0-9_]*\>\ze\s*::/
syntax match flechaLabel /#\s*[A-Za-z_][A-Za-z0-9_]*/
syntax match flechaJumpTarget /\<[A-Za-z_][A-Za-z0-9_]*\>/ contained

syntax match flechaNumber /\<\%(0[xX][0-9A-Fa-f]\+\|\d\+\)\>/
syntax match flechaFloat /\<\d\+\.\d\+\>/

syntax match flechaEscape /\\[ntr\\"]/ contained
syntax match flechaCharEscape /\\[ntr0\\']/ contained
syntax region flechaString start=/"/ skip=/\\./ end=/"/ contains=flechaEscape
syntax region flechaCharacter start=/'/ skip=/\\./ end=/'/ contains=flechaCharEscape

syntax keyword flechaTodo TODO FIXME XXX NOTE contained
syntax match flechaLineComment /\/\/.*$/ contains=flechaTodo
syntax region flechaBlockComment start=/\/\*/ end=/\*\// contains=flechaTodo

" Prefer Neovim's semantic highlight groups when they exist. Traditional Vim
" groups remain the fallback, so no color is selected by this syntax file.
function! s:FollowTheme(group, semantic_group, fallback_group) abort
    let l:target = hlexists(a:semantic_group)
                \ ? a:semantic_group
                \ : a:fallback_group
    execute 'highlight default link ' . a:group . ' ' . l:target
endfunction

call s:FollowTheme('flechaImports', '@keyword.import', 'Include')
call s:FollowTheme('flechaSelf', '@variable.builtin', 'Special')
call s:FollowTheme('@lsp.type.self.flecha', '@variable.builtin', 'Special')
call s:FollowTheme('flechaTest', '@keyword', 'Keyword')
call s:FollowTheme('flechaFuncKeyword', '@keyword.function', 'Keyword')
call s:FollowTheme('flechaFloatConstant', '@constant', 'Constant')
call s:FollowTheme('flechaIntegerLimit', '@constant', 'Constant')
call s:FollowTheme('flechaCrashableBang', '@operator', 'Operator')
call s:FollowTheme('flechaCrashBuiltin', '@function.builtin', 'Function')
call s:FollowTheme('flechaType', '@type', 'Type')
call s:FollowTheme('flechaBuiltinTypeSet', '@type.builtin', 'Type')
call s:FollowTheme('flechaTypeSetName', '@type.definition', 'Type')
call s:FollowTheme('flechaFunction', '@function', 'Function')
call s:FollowTheme('flechaMethod', '@function.method', 'Function')
call s:FollowTheme('flechaParameter', '@variable.parameter', 'Identifier')
call s:FollowTheme('flechaVariable', '@variable', 'Identifier')
call s:FollowTheme('flechaNamedConstant', '@constant', 'Constant')
call s:FollowTheme('flechaEnumConstant', '@constant', 'Constant')
call s:FollowTheme('flechaTypeSetKeyword', '@keyword.type', 'Keyword')
call s:FollowTheme('flechaModuleName', '@module', 'Identifier')
call s:FollowTheme('flechaNamespace', '@module', 'Identifier')
call s:FollowTheme('flechaModifier', '@keyword.modifier', 'StorageClass')
call s:FollowTheme('flechaConditional', '@keyword.conditional', 'Conditional')
call s:FollowTheme('flechaRepeat', '@keyword.repeat', 'Repeat')
call s:FollowTheme('flechaStatement', '@keyword', 'Statement')
call s:FollowTheme('flechaNamespaceAttachment', '@keyword', 'Statement')
call s:FollowTheme('flechaJump', '@keyword', 'Statement')
call s:FollowTheme('flechaCast', '@keyword.operator', 'Keyword')
call s:FollowTheme('flechaBoolean', '@boolean', 'Boolean')
call s:FollowTheme('flechaConstant', '@constant.builtin', 'Constant')
call s:FollowTheme('flechaWordOperator', '@operator', 'Operator')
call s:FollowTheme('flechaOperator', '@operator', 'Operator')
call s:FollowTheme('flechaInheritanceOperator', '@keyword.operator', 'Keyword')
call s:FollowTheme('flechaRawAddress', '@keyword', 'Keyword')
call s:FollowTheme('flechaCastMethod', '@function.method', 'Function')
call s:FollowTheme('flechaIsOfMethod', '@function.method', 'Function')
call s:FollowTheme('flechaAllocation', '@function.builtin', 'Function')
call s:FollowTheme('flechaLabel', '@label', 'Label')
call s:FollowTheme('flechaJumpTarget', '@label', 'Label')
call s:FollowTheme('flechaNumber', '@number', 'Number')
call s:FollowTheme('flechaFloat', '@number.float', 'Float')
call s:FollowTheme('flechaString', '@string', 'String')
call s:FollowTheme('flechaCharacter', '@character', 'Character')
call s:FollowTheme('flechaEscape', '@string.escape', 'SpecialChar')
call s:FollowTheme('flechaCharEscape', '@string.escape', 'SpecialChar')
call s:FollowTheme('flechaLineComment', '@comment', 'Comment')
call s:FollowTheme('flechaBlockComment', '@comment', 'Comment')
call s:FollowTheme('flechaTodo', '@comment.todo', 'Todo')

delfunction s:FollowTheme

let b:current_syntax = 'flecha'
