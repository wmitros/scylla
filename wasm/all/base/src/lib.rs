use std::collections::{BTreeSet, BTreeMap};
extern "C" {
    fn malloc(size: usize) -> usize;
    fn free(ptr: *mut usize);
}
#[no_mangle]
pub unsafe extern "C" fn _scylla_malloc(size: usize) -> u32 {
    malloc(size) as u32
}

#[no_mangle]
pub unsafe extern "C" fn _scylla_free(ptr: *mut usize) {
    free(ptr)
}

pub fn swap_int32(val: u32) -> u32 {
    (val >> 24) | ((val & 0x00ff0000) >> 8) | ((val & 0x0000ff00) << 8) | (val << 24)
}

pub fn swap_int64(val: u64) -> u64 {
    let mut val = ((val << 8) & 0xFF00FF00FF00FF00u64 ) | ((val >> 8) & 0x00FF00FF00FF00FFu64 );
    val = ((val << 16) & 0xFFFF0000FFFF0000u64 ) | ((val >> 16) & 0x0000FFFF0000FFFFu64 );
    (val << 32) | (val >> 32)
}

fn size_ptr(size: u32, ptr: u32) -> u64 {
    ((size as u64) << 32) + ptr as u64
}

pub trait FromCQL {
    fn from_cql(sizeptr: u64) -> Self where Self: Sized {
        let ptr = (sizeptr & 0xffffffff) as *mut usize;
        let ret = Self::deserialize_cql(sizeptr);
        unsafe { _scylla_free(ptr) };
        ret
    }
    fn deserialize_cql(sizeptr: u64) -> Self;
}

pub trait IntoCQL {
    fn into_cql(&self) -> u64 {
        let siz = self.size_cql();
        unsafe {
            let dest = _scylla_malloc(siz);
            let mut dest_slice = std::slice::from_raw_parts_mut(dest as *mut u8, siz);
            self.serialize_cql(&mut dest_slice);
            size_ptr(siz as u32, dest)
        }
    }
    fn size_cql(&self) -> usize;
    fn serialize_cql(&self, dest: &mut [u8]);
}

impl FromCQL for String {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let size = (sizeptr >> 32) as usize;
        let ptr = (sizeptr & 0xffffffff) as *mut u8;
        let vec : Vec<u8> = unsafe { std::slice::from_raw_parts(ptr, size).to_vec() };
        match String::from_utf8(vec) {
            Ok(v) => {v},
            Err(_e) => {panic!("string is not utf8 encoded")},
        }
    }
}

impl IntoCQL for String {
    fn serialize_cql(&self, dest: &mut [u8]) {
        dest.copy_from_slice(self.as_bytes());
    }
    fn size_cql(&self) -> usize {
        self.len()
    }
}

impl<T: FromCQL> FromCQL for Vec<T> {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let size = (sizeptr >> 32) as usize;
        let ptr = (sizeptr & 0xffffffff) as *mut u8;
        let mut vec = Vec::new();
        let length = u32::from_be(unsafe {*(ptr as *const u32)});
        let mut curr_ptr = (ptr as u32) + 4;
        for _ in 0..length {
            let siz = u32::from_be(unsafe {*(curr_ptr as *const u32)});
            curr_ptr += 4;
            vec.push(T::deserialize_cql(size_ptr(siz, curr_ptr)));
            curr_ptr += siz;
        }
        if curr_ptr != (ptr as u32) + size as u32 {
            panic!("data has different size than specified")
        }
        vec
    }
}

impl<T: IntoCQL> IntoCQL for Vec<T> {
    fn serialize_cql(&self, dest: &mut [u8]) {
        let length = self.len() as u32;
        dest[0..4].copy_from_slice(&length.to_be_bytes());
        let mut offset = 4;
        for it in self.iter() {
            let siz = it.size_cql() as u32;
            dest[offset..(offset+4)].copy_from_slice(&siz.to_be_bytes());
            offset += 4;
            it.serialize_cql(&mut dest[offset..(offset+(siz as usize))]);
            offset += siz as usize;
        }
    }
    fn size_cql(&self) -> usize {
        let mut ret : usize = 4;
        for it in self.iter() {
            ret += 4 + it.size_cql();
        }
        ret
    }
}

impl<K: FromCQL + std::cmp::Ord, V: FromCQL> FromCQL for BTreeMap<K,V> {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let size = (sizeptr >> 32) as usize;
        let ptr = (sizeptr & 0xffffffff) as *mut u8;
        let mut map = BTreeMap::new();
        let length = u32::from_be(unsafe {*(ptr as *const u32)});
        let mut curr_ptr = (ptr as u32) + 4;
        for _ in 0..length {
            let siz = u32::from_be(unsafe {*(curr_ptr as *const u32)});
            curr_ptr += 4;
            let key = K::deserialize_cql(size_ptr(siz, curr_ptr));
            curr_ptr += siz;
            let siz = u32::from_be(unsafe {*(curr_ptr as *const u32)});
            curr_ptr += 4;
            let val = V::deserialize_cql(size_ptr(siz, curr_ptr));
            curr_ptr += siz;
            map.insert(key, val);
        }
        if curr_ptr != (ptr as u32) + size as u32 {
            panic!("data has different size than specified")
        }
        map
    }
}
impl<T: FromCQL + std::cmp::Ord> FromCQL for BTreeSet<T> {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let size = (sizeptr >> 32) as usize;
        let ptr = (sizeptr & 0xffffffff) as *mut u8;
        let mut vec = BTreeSet::new();
        let length = u32::from_be(unsafe {*(ptr as *const u32)});
        let mut curr_ptr = (ptr as u32) + 4;
        for _ in 0..length {
            let siz = u32::from_be(unsafe {*(curr_ptr as *const u32)});
            curr_ptr += 4;
            vec.insert(T::deserialize_cql(size_ptr(siz, curr_ptr)));
            curr_ptr += siz;
        }
        if curr_ptr != (ptr as u32) + size as u32 {
            panic!("data has different size than specified")
        }
        vec
    }
}

impl<T: IntoCQL + std::cmp::Ord> IntoCQL for BTreeSet<T> {
    fn serialize_cql(&self, dest: &mut [u8]) {
        let length = self.len() as u32;
        dest[0..4].copy_from_slice(&length.to_be_bytes());
        let mut offset = 4;
        for it in self.iter() {
            let siz = it.size_cql() as u32;
            dest[offset..(offset+4)].copy_from_slice(&siz.to_be_bytes());
            offset += 4;
            it.serialize_cql(&mut dest[offset..(offset+(siz as usize))]);
            offset += siz as usize;
        }
    }
    fn size_cql(&self) -> usize {
        let mut ret : usize = 4;
        for it in self.iter() {
            ret += 4 + it.size_cql();
        }
        ret
    }
}


impl<K: IntoCQL + std::cmp::Ord, V: IntoCQL> IntoCQL for BTreeMap<K,V> {
    fn serialize_cql(&self, dest: &mut [u8]) {
        let length = self.len() as u32;
        dest[0..4].copy_from_slice(&length.to_be_bytes());
        let mut offset = 4;
        for it in self.iter() {
            let siz = it.0.size_cql() as u32;
            dest[offset..(offset+4)].copy_from_slice(&siz.to_be_bytes());
            offset += 4;
            it.0.serialize_cql(&mut dest[offset..(offset+(siz as usize))]);
            offset += siz as usize;
            let siz = it.1.size_cql() as u32;
            dest[offset..(offset+4)].copy_from_slice(&siz.to_be_bytes());
            offset += 4;
            it.1.serialize_cql(&mut dest[offset..(offset+(siz as usize))]);
            offset += siz as usize;
        }
    }
    fn size_cql(&self) -> usize {
        let mut ret : usize = 4;
        for it in self.iter() {
            ret += 4 + it.0.size_cql();
            ret += 4 + it.1.size_cql();
        }
        ret
    }
}

// impl<T: FromCQL> FromCQL for Option<T> {
//     fn deserialize_cql(sizeptr: u64) -> Self {
//         let size = (sizeptr >> 32) as usize;
//         let ptr = (sizeptr & 0xffffffff) as *mut u8;
//         if size == -1 {
//             None
//         } else {
//             T::deserialize_cql(sizeptr)
//         }
//     }
// }

// impl<T: IntoCQL> IntoCQL for Option<T> {
//     fn serialize_cql(&self, dest: &mut [u8]) {
//         if let None = *self {

//         }
//         let length = self.len() as u32;
//         dest[0..4].copy_from_slice(&length.to_be_bytes());
//         let mut offset = 4;
//         for it in self.iter() {
//             let siz = it.size_cql() as u32;
//             dest[offset..(offset+4)].copy_from_slice(&siz.to_be_bytes());
//             offset += 4;
//             it.serialize_cql(&mut dest[offset..(offset+(siz as usize))]);
//             offset += siz as usize;
//         }
//     }
//     fn size_cql(&self) -> usize {
//         if let None = *self {
            
//         }
//         let mut ret : usize = 4;
//         for it in self.iter() {
//             ret += 4 + it.size_cql();
//         }
//         ret
//     }
// }
