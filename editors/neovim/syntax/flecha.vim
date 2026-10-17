if exists('b:current_syntax')
    finish
endif

syntax case match

syntax match flechaImports /@imports\>/

syntax keyword flechaType i8 i16 i32 i64 isize
syntax keyword flechaType u8 u16 u32 u64 usize
syntax keyword flechaType f32 f64 bool char void string stringv error AllocationError

syntax keyword flechaModifier unsafe export extern inline comptime own lend const constptr constref volatile private override
syntax keyword flechaTest test
syntax keyword flechaConditional if else also match
syntax keyword flechaRepeat while for loop range
syntax keyword flechaStatement return yield proceed break defer module enum structure build
syntax keyword flechaJump jumpto nextgroup=flechaJumpTarget skipwhite
syntax keyword flechaCast as
syntax keyword flechaBoolean true false
syntax keyword flechaConstant none null
syntax keyword flechaSelf self
syntax keyword flechaWordOperator and or

syntax match flechaOperator /::\|<-\|->\|=>\|:=\|===\|!==\|==\|!=\|<=\|>=\|&&\|||\|[&=+*\/%^!<>-]/
syntax match flechaRawAddress /\%(&\s*\)\@<=raw\>/
syntax match flechaFunction /\<[A-Za-z_][A-Za-z0-9_]*\>\ze\s*\%(<.\{-}>\s*\)\?(/
syntax match flechaAllocation /\<new\>\ze\s*</
syntax match flechaAllocation /\<take\>\ze\s*(/
syntax match flechaCastMethod /\.cast\>\ze\s*</
syntax match flechaLabel /#\s*[A-Za-z_][A-Za-z0-9_]*/
syntax match flechaJumpTarget /\<[A-Za-z_][A-Za-z0-9_]*\>/ contained

syntax match flechaNumber /\<\d\+\>/
syntax match flechaFloat /\<\d\+\.\d\+\>/

syntax match flechaEscape /\\[ntr\\"]/ contained
syntax match flechaCharEscape /\\[ntr0\\']/ contained
syntax region flechaString start=/"/ skip=/\\./ end=/"/ contains=flechaEscape
syntax region flechaCharacter start=/'/ skip=/\\./ end=/'/ contains=flechaCharEscape

syntax keyword flechaTodo TODO FIXME XXX NOTE contained
syntax match flechaLineComment /\/\/.*$/ contains=flechaTodo
syntax region flechaBlockComment start=/\/\*/ end=/\*\// contains=flechaTodo

highlight default flechaImports guifg=#87CEFA ctermfg=117
highlight default flechaSelf guifg=#5F87AF ctermfg=67
highlight default flechaTest guifg=#D4AF37 ctermfg=178
highlight default link flechaType Type
highlight default link flechaModifier StorageClass
highlight default link flechaConditional Conditional
highlight default link flechaRepeat Repeat
highlight default link flechaStatement Statement
highlight default link flechaJump Statement
highlight default link flechaCast Keyword
highlight default link flechaBoolean Boolean
highlight default link flechaConstant Constant
highlight default link flechaWordOperator Operator
highlight default link flechaOperator Operator
highlight default link flechaRawAddress Keyword
highlight default link flechaFunction Function
highlight default link flechaCastMethod Function
highlight default link flechaAllocation Function
highlight default link flechaLabel Label
highlight default link flechaJumpTarget Label
highlight default link flechaNumber Number
highlight default link flechaFloat Float
highlight default link flechaString String
highlight default link flechaCharacter Character
highlight default link flechaEscape SpecialChar
highlight default link flechaCharEscape SpecialChar
highlight default link flechaLineComment Comment
highlight default link flechaBlockComment Comment
highlight default link flechaTodo Todo

let b:current_syntax = 'flecha'
