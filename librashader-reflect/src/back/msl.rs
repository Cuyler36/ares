use crate::back::targets::MSL;
use crate::back::{CompileReflectShader, CompilerBackend, FromCompilation};
use crate::error::ShaderReflectError;
use crate::front::SpirvCompilation;
use crate::reflect::cross::msl::MslReflect;
use crate::reflect::cross::{CompiledProgram, SpirvCross};
use crate::reflect::naga::{Naga, NagaReflect};
use naga::back::msl::TranslationInfo;
use naga::Module;

/// The MSL language version to target.
pub use spirv_cross2::compile::msl::MslVersion;

/// Compiler options for MSL
#[derive(Debug, Default, Clone)]
pub struct MslNagaCompileOptions {
    // pub write_pcb_as_ubo: bool,
    pub sampler_bind_group: u32,
}

/// The context for a MSL compilation via spirv-cross.
pub struct CrossMslContext {
    /// The compiled HLSL program.
    pub artifact: CompiledProgram<spirv_cross2::targets::Msl>,
}

#[cfg(not(feature = "stable"))]
impl FromCompilation<SpirvCompilation, SpirvCross> for MSL {
    type Target = MSL;
    type Options = Option<self::MslVersion>;
    type Context = CrossMslContext;
    type Output = impl CompileReflectShader<Self::Target, SpirvCompilation, SpirvCross>;

    fn from_compilation(
        compile: SpirvCompilation,
    ) -> Result<CompilerBackend<Self::Output>, ShaderReflectError> {
        Ok(CompilerBackend {
            backend: MslReflect::try_from(&compile)?,
        })
    }
}

#[cfg(feature = "stable")]
impl FromCompilation<SpirvCompilation, SpirvCross> for MSL {
    type Target = MSL;
    type Options = Option<self::MslVersion>;
    type Context = CrossMslContext;
    type Output = Box<dyn CompileReflectShader<Self::Target, SpirvCompilation, SpirvCross> + Send>;

    fn from_compilation(
        compile: SpirvCompilation,
    ) -> Result<CompilerBackend<Self::Output>, ShaderReflectError> {
        Ok(CompilerBackend {
            backend: Box::new(MslReflect::try_from(&compile)?),
        })
    }
}

/// The naga module for a shader after compilation
pub struct NagaMslModule {
    pub translation_info: TranslationInfo,
    pub module: Module,
}

pub struct NagaMslContext {
    pub vertex: NagaMslModule,
    pub fragment: NagaMslModule,
    pub next_free_binding: u32,
}

#[cfg(not(feature = "stable"))]
impl FromCompilation<SpirvCompilation, Naga> for MSL {
    type Target = MSL;
    type Options = Option<self::MslVersion>;
    type Context = NagaMslContext;
    type Output = impl CompileReflectShader<Self::Target, SpirvCompilation, Naga>;

    fn from_compilation(
        compile: SpirvCompilation,
    ) -> Result<CompilerBackend<Self::Output>, ShaderReflectError> {
        Ok(CompilerBackend {
            backend: NagaReflect::try_from(&compile)?,
        })
    }
}

#[cfg(feature = "stable")]
impl FromCompilation<SpirvCompilation, Naga> for MSL {
    type Target = MSL;
    type Options = Option<self::MslVersion>;
    type Context = NagaMslContext;
    type Output = Box<dyn CompileReflectShader<Self::Target, SpirvCompilation, Naga> + Send>;

    fn from_compilation(
        compile: SpirvCompilation,
    ) -> Result<CompilerBackend<Self::Output>, ShaderReflectError> {
        Ok(CompilerBackend {
            backend: Box::new(NagaReflect::try_from(&compile)?),
        })
    }
}
