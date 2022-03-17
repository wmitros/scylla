#![feature(rustc_private)]
use quote::quote;
use syn::{ItemFn, FnArg, Pat, Stmt, parse, parse_str};
use proc_macro::TokenStream;
use proc_macro2::Span;
use std::iter::FromIterator;
use quote::ToTokens;
use std::borrow::Borrow;
use std::borrow::BorrowMut;
#[proc_macro_attribute]
pub fn scylla_bindgen(attrs: TokenStream, item: TokenStream) -> TokenStream {
    if let Ok(input) = parse::<ItemFn>(item.clone()) {
        let mut decl1 = input.sig.clone();
        let mut decl2 = input.sig;
        decl1.ident = syn::Ident::new(format!("{}{}", "_scylla_internal_", decl2.ident.to_string()).as_str(), Span::call_site());
        for inp in &mut decl2.inputs {
            if let &mut FnArg::Typed(ref mut pat) = inp {
                pat.ty = Box::new(syn::Type::from(parse_str::<syn::TypePath>("u64").expect("code generation error")));
                if let &mut Pat::Ident(ref mut id) = pat.pat.borrow_mut() {
                    id.mutability = Option::None;
                }
            }
        }
        decl2.output = syn::ReturnType::from(syn::ReturnType::Type(syn::token::RArrow::default(),
            Box::new(syn::Type::from(parse_str::<syn::TypePath>("u64").expect("code generation error")))));
        let iblock = &input.block;
        let mut block = syn::Block{brace_token: input.block.brace_token.clone(), stmts: Vec::<Stmt>::new()};
        let mut exp = parse_str::<syn::Expr>(&format!(
            "{}()", &decl1.ident.to_string())).expect("code generation error");
        if let &mut syn::Expr::Call(ref mut c) = &mut exp {
            for inp in &decl1.inputs {
                if let &FnArg::Typed(ref pat) = inp {
                    if let &Pat::Ident(ref ppat) = pat.pat.borrow() {
                        let typestr = pat.ty.to_owned().into_token_stream().to_string();
                        let fromstring = format!("<{}>::from_cql({})", typestr, ppat.ident.to_string());
                        c.args.push(parse_str::<syn::Expr>(&fromstring).expect("code generation error"));
                    }
                }
            }
        }
        let exprstr = exp.into_token_stream().to_string();
        let retexp = parse_str::<syn::Expr>(&format!("{}.into_cql()", exprstr)).expect("code generation error");
        block.stmts.push(Stmt::from(Stmt::Expr(retexp)));
        let mut ts0 = TokenStream::from(quote! {
            extern crate base;
            use crate::base::*;
        });
        let ts05 = TokenStream::from(quote! {
            #[no_mangle]
            pub static _scylla_abi: u32 = 1;
        });
        ts0.extend(ts05.into_iter());
        let mut ts = TokenStream::from_iter(attrs);
        let ts2 = TokenStream::from(quote! {
                #decl1
                #iblock

                #[no_mangle]
                pub extern "C" #decl2
                #block
            });
        ts.extend(ts2.into_iter());
        ts0.extend(ts.into_iter());
        ts0
    } else {
        if true {
            item
        } else {
        TokenStream::from(
            syn::Error::new(
                Span::call_site(),
                "scylla_bindgen can only be used on functions.",
            )
            .to_compile_error(),
        )}
    }
}
