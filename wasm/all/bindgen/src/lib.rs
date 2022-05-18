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
    let scalar_types = vec!["i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "bool", "char", "f32", "f64"];
    let native_types = vec!["u32", "u64", "i32", "i64", "f32", "f64"];
    if let Ok(input) = parse::<ItemFn>(item.clone()) {
        let mut decl1 = input.sig;
        let mut decl2 = decl1.clone();
        decl1.ident = syn::Ident::new(format!("{}{}", "_scylla_internal_", decl2.ident.to_string()).as_str(), Span::call_site());
        for inp in &mut decl2.inputs {
            if let &mut FnArg::Typed(ref mut pat) = inp {
                let typestr = pat.ty.to_owned().into_token_stream().to_string();
                if !scalar_types.iter().any(|&s| s == &typestr) {
                    pat.ty = Box::new(syn::Type::from(parse_str::<syn::TypePath>("u64").expect("code generation error")));
                } else if !native_types.iter().any(|&s| s == &typestr) {
                    if typestr.starts_with("i") {
                        pat.ty = Box::new(syn::Type::from(parse_str::<syn::TypePath>("i32").expect("code generation error")));
                    } else {
                        pat.ty = Box::new(syn::Type::from(parse_str::<syn::TypePath>("u32").expect("code generation error")));
                    }
                }
                if let &mut Pat::Ident(ref mut id) = pat.pat.borrow_mut() {
                    id.mutability = Option::None;
                }
            }
        }
        let output_typestr;
        if let &syn::ReturnType::Type(_, ref typ) = &(decl2.output) {
            output_typestr = typ.to_owned().into_token_stream().to_string();
            if !scalar_types.iter().any(|&s| s == &output_typestr) {
                decl2.output = syn::ReturnType::from(syn::ReturnType::Type(syn::token::RArrow::default(),
                Box::new(syn::Type::from(parse_str::<syn::TypePath>("u64").expect("code generation error")))));
            } else if !native_types.iter().any(|&s| s == &output_typestr) {
                if output_typestr.starts_with("i") {
                    decl2.output = syn::ReturnType::from(syn::ReturnType::Type(syn::token::RArrow::default(),
                    Box::new(syn::Type::from(parse_str::<syn::TypePath>("i32").expect("code generation error")))));
                } else {
                    decl2.output = syn::ReturnType::from(syn::ReturnType::Type(syn::token::RArrow::default(),
                    Box::new(syn::Type::from(parse_str::<syn::TypePath>("u32").expect("code generation error")))));
                }
            }
        } else {
            output_typestr = "".to_string();
        }
        let iblock = &input.block;
        let mut block = syn::Block{brace_token: input.block.brace_token.clone(), stmts: Vec::<Stmt>::new()};
        let mut exp = parse_str::<syn::Expr>(&format!(
            "{}()", &decl1.ident.to_string())).expect("code generation error");
        if let &mut syn::Expr::Call(ref mut c) = &mut exp {
            for inp in &decl1.inputs {
                if let &FnArg::Typed(ref pat) = inp {
                    if let &Pat::Ident(ref ppat) = pat.pat.borrow() {
                        let typestr = pat.ty.to_owned().into_token_stream().to_string();
                        let fromstring;
                        if !scalar_types.iter().any(|&s| s == &typestr) {
                            fromstring = format!("<{}>::from_cql({})", typestr, ppat.ident.to_string());
                        } else if native_types.iter().any(|&s| s == &typestr) {
                            fromstring = ppat.ident.to_string();
                        } else {
                            fromstring = format!("{} as {}", ppat.ident.to_string(), typestr);
                        }
                        c.args.push(parse_str::<syn::Expr>(&fromstring).expect("code generation error"));
                    }
                }
            }
        }
        let exprstr = exp.into_token_stream().to_string();
        let retexp;
        if !scalar_types.iter().any(|&s| s == &output_typestr) {
            retexp = parse_str::<syn::Expr>(&format!("{}.into_cql()", exprstr)).expect("code generation error");
        } else if !native_types.iter().any(|&s| s == &output_typestr) {
            if output_typestr.starts_with("i") {
                retexp = parse_str::<syn::Expr>(&format!("{} as i32", exprstr)).expect("code generation error");
            } else {
                retexp = parse_str::<syn::Expr>(&format!("{} as u32", exprstr)).expect("code generation error");
            }
        } else {
            retexp = parse_str::<syn::Expr>(&exprstr).expect("code generation error");
        }
        block.stmts.push(Stmt::from(Stmt::Expr(retexp)));
        let mut ts0 = TokenStream::from(quote! {
            extern crate base;
            use crate::base::*;
        });
        let ts05 = TokenStream::from(quote! {
            #[no_mangle]
            pub static _scylla_abi: u32 = 2;
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
        TokenStream::from(
            syn::Error::new(
                Span::call_site(),
                "scylla_bindgen can only be used on functions.",
            )
            .to_compile_error(),
        )
    }
}
