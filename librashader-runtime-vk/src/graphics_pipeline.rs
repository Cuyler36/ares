use crate::{error, util};
use ash::vk;

use crate::error::FilterChainError;
use crate::framebuffer::OutputImage;
use crate::render_pass::VulkanRenderPass;
use ash::vk::PushConstantRange;
use bytemuck::offset_of;
use librashader_cache::cache_pipeline;
use librashader_common::map::FastHashMap;
use librashader_reflect::back::ShaderCompilerOutput;
use librashader_reflect::reflect::semantics::{BufferReflection, TextureBinding};
use librashader_reflect::reflect::ShaderReflection;
use librashader_runtime::quad::VertexInput;
use librashader_runtime::render_target::RenderTarget;
use std::ffi::CStr;
use std::sync::Arc;

const ENTRY_POINT: &CStr = unsafe { CStr::from_bytes_with_nul_unchecked(b"main\0") };

pub struct PipelineDescriptors<'a> {
    pub replicas: u32,
    pub layout_bindings: Vec<vk::DescriptorSetLayoutBinding<'a>>,
    pub pool_sizes: Vec<vk::DescriptorPoolSize>,
}

impl PipelineDescriptors<'_> {
    pub fn new(duplicates: u32) -> Self {
        Self {
            replicas: duplicates,
            layout_bindings: vec![],
            pool_sizes: vec![],
        }
    }

    pub fn add_ubo_binding(&mut self, ubo_meta: Option<&BufferReflection<u32>>) {
        if let Some(ubo_meta) = ubo_meta.filter(|ubo_meta| !ubo_meta.stage_mask.is_empty()) {
            let ubo_mask = util::binding_stage_to_vulkan_stage(ubo_meta.stage_mask);

            self.layout_bindings.push(vk::DescriptorSetLayoutBinding {
                binding: ubo_meta.binding,
                descriptor_type: vk::DescriptorType::UNIFORM_BUFFER,
                descriptor_count: 1,
                stage_flags: ubo_mask,
                p_immutable_samplers: std::ptr::null(),
                _marker: Default::default(),
            });

            self.pool_sizes.push(vk::DescriptorPoolSize {
                ty: vk::DescriptorType::UNIFORM_BUFFER,
                descriptor_count: self.replicas,
            })
        }
    }

    pub fn add_texture_bindings<'a>(&mut self, textures: impl Iterator<Item = &'a TextureBinding>) {
        let texture_mask = vk::ShaderStageFlags::FRAGMENT;
        for texture in textures {
            self.layout_bindings.push(vk::DescriptorSetLayoutBinding {
                binding: texture.binding,
                descriptor_type: vk::DescriptorType::COMBINED_IMAGE_SAMPLER,
                descriptor_count: 1,
                stage_flags: texture_mask,
                p_immutable_samplers: std::ptr::null(),
                _marker: Default::default(),
            });

            self.pool_sizes.push(vk::DescriptorPoolSize {
                ty: vk::DescriptorType::COMBINED_IMAGE_SAMPLER,
                descriptor_count: self.replicas,
            })
        }
    }

    pub fn bindings(&self) -> &[vk::DescriptorSetLayoutBinding] {
        self.layout_bindings.as_ref()
    }

    pub fn create_descriptor_set_layout(
        &self,
        device: &ash::Device,
    ) -> error::Result<vk::DescriptorSetLayout> {
        unsafe {
            let layout = device.create_descriptor_set_layout(
                &vk::DescriptorSetLayoutCreateInfo::default().bindings(self.bindings()),
                None,
            )?;
            Ok(layout)
        }
    }
}

pub struct PipelineLayoutObjects {
    pub layout: vk::PipelineLayout,
    pub descriptor_sets: Vec<vk::DescriptorSet>,
    pub descriptor_sets_alt: Vec<vk::DescriptorSet>,

    pub _pool: vk::DescriptorPool,
    pub _descriptor_set_layout: [vk::DescriptorSetLayout; 1],
}

impl PipelineLayoutObjects {
    pub fn new(
        reflection: &ShaderReflection,
        replicas: u32,
        device: &ash::Device,
    ) -> error::Result<Self> {
        let mut descriptors = PipelineDescriptors::new(replicas);
        descriptors.add_ubo_binding(reflection.ubo.as_ref());
        descriptors.add_texture_bindings(reflection.meta.texture_meta.values());

        let descriptor_set_layout = [descriptors.create_descriptor_set_layout(device)?];

        let pipeline_create_info =
            vk::PipelineLayoutCreateInfo::default().set_layouts(&descriptor_set_layout);

        let push_constant_range = reflection.push_constant.as_ref().map(|push_constant| {
            let stage_mask = util::binding_stage_to_vulkan_stage(push_constant.stage_mask);
            [vk::PushConstantRange::default()
                .stage_flags(stage_mask)
                .size(push_constant.size)]
        });

        let push_constant_range: &[PushConstantRange] =
            push_constant_range.as_ref().map_or(&[], |o| o);

        let pipeline_create_info = pipeline_create_info.push_constant_ranges(push_constant_range);

        let layout = unsafe { device.create_pipeline_layout(&pipeline_create_info, None)? };

        let pool_info = vk::DescriptorPoolCreateInfo::default()
            .max_sets(replicas * 2)
            .pool_sizes(&descriptors.pool_sizes);

        let pool = unsafe { device.create_descriptor_pool(&pool_info, None)? };

        let mut descriptor_sets = Vec::new();
        let alloc_info = vk::DescriptorSetAllocateInfo::default()
            .descriptor_pool(pool)
            .set_layouts(&descriptor_set_layout);

        for _ in 0..replicas {
            let set = unsafe { device.allocate_descriptor_sets(&alloc_info)? };
            descriptor_sets.push(set)
        }

        let descriptor_sets: Vec<vk::DescriptorSet> =
            descriptor_sets.into_iter().flatten().collect();

        let mut descriptor_sets_alt = Vec::new();
        let alloc_info = vk::DescriptorSetAllocateInfo::default()
            .descriptor_pool(pool)
            .set_layouts(&descriptor_set_layout);

        for _ in 0..replicas {
            let set = unsafe { device.allocate_descriptor_sets(&alloc_info)? };
            descriptor_sets_alt.push(set)
        }

        let descriptor_sets_alt: Vec<vk::DescriptorSet> =
            descriptor_sets_alt.into_iter().flatten().collect();

        Ok(PipelineLayoutObjects {
            layout,
            _descriptor_set_layout: descriptor_set_layout,
            descriptor_sets,
            descriptor_sets_alt,
            _pool: pool,
        })
    }
}

pub struct VulkanShaderModule {
    shader: vk::ShaderModule,
    device: ash::Device,
}

impl VulkanShaderModule {
    pub fn new(
        device: &ash::Device,
        info: &vk::ShaderModuleCreateInfo,
    ) -> error::Result<VulkanShaderModule> {
        Ok(VulkanShaderModule {
            shader: unsafe { device.create_shader_module(info, None)? },
            device: device.clone(),
        })
    }
}

impl Drop for VulkanShaderModule {
    fn drop(&mut self) {
        unsafe { self.device.destroy_shader_module(self.shader, None) }
    }
}

pub struct VulkanGraphicsPipeline {
    pub layout: PipelineLayoutObjects,
    pub pipelines: FastHashMap<vk::Format, vk::Pipeline>,
    pub render_passes: FastHashMap<vk::Format, Option<VulkanRenderPass>>,
    device: Arc<ash::Device>,
    vertex: VulkanShaderModule,
    fragment: VulkanShaderModule,
    cache: vk::PipelineCache,
    use_render_pass: bool,
}

impl VulkanGraphicsPipeline {
    fn create_pipeline(
        device: &ash::Device,
        cache: &vk::PipelineCache,
        pipeline_layout: &PipelineLayoutObjects,
        vertex_module: &VulkanShaderModule,
        fragment_module: &VulkanShaderModule,
        render_pass: Option<&VulkanRenderPass>,
    ) -> error::Result<vk::Pipeline> {
        let input_assembly = vk::PipelineInputAssemblyStateCreateInfo::default()
            .topology(vk::PrimitiveTopology::TRIANGLE_STRIP);

        let vao_state = [
            vk::VertexInputAttributeDescription {
                location: 0,
                binding: 0,
                format: vk::Format::R32G32_SFLOAT,
                offset: offset_of!(VertexInput, position) as u32,
            },
            vk::VertexInputAttributeDescription {
                location: 1,
                binding: 0,
                format: vk::Format::R32G32_SFLOAT,
                offset: offset_of!(VertexInput, texcoord) as u32,
            },
        ];

        let input_binding = vk::VertexInputBindingDescription::default()
            .binding(0)
            .stride(std::mem::size_of::<VertexInput>() as u32)
            .input_rate(vk::VertexInputRate::VERTEX);

        let input_binding = [input_binding];
        let pipeline_input_state = vk::PipelineVertexInputStateCreateInfo::default()
            .vertex_binding_descriptions(&input_binding)
            .vertex_attribute_descriptions(&vao_state);

        let raster_state = vk::PipelineRasterizationStateCreateInfo::default()
            .polygon_mode(vk::PolygonMode::FILL)
            .cull_mode(vk::CullModeFlags::NONE)
            .front_face(vk::FrontFace::COUNTER_CLOCKWISE)
            .depth_clamp_enable(false)
            .rasterizer_discard_enable(false)
            .depth_bias_enable(false)
            .line_width(1.0);

        let attachments = vk::PipelineColorBlendAttachmentState::default()
            .blend_enable(false)
            .color_write_mask(vk::ColorComponentFlags::from_raw(0xf));

        let attachments = [attachments];
        let blend_state =
            vk::PipelineColorBlendStateCreateInfo::default().attachments(&attachments);

        let viewport_state = vk::PipelineViewportStateCreateInfo::default()
            .viewport_count(1)
            .scissor_count(1);

        let depth_stencil_state = vk::PipelineDepthStencilStateCreateInfo::default()
            .depth_test_enable(false)
            .depth_write_enable(false)
            .stencil_test_enable(false)
            .depth_bounds_test_enable(false)
            .min_depth_bounds(1.0)
            .max_depth_bounds(1.0);

        let multisample_state = vk::PipelineMultisampleStateCreateInfo::default()
            .rasterization_samples(vk::SampleCountFlags::TYPE_1);

        let states = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
        let dynamic_state = vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&states);

        let shader_stages = [
            vk::PipelineShaderStageCreateInfo::default()
                .stage(vk::ShaderStageFlags::VERTEX)
                .name(ENTRY_POINT)
                .module(vertex_module.shader),
            vk::PipelineShaderStageCreateInfo::default()
                .stage(vk::ShaderStageFlags::FRAGMENT)
                .name(ENTRY_POINT)
                .module(fragment_module.shader),
        ];

        let mut pipeline_info = vk::GraphicsPipelineCreateInfo::default()
            .stages(&shader_stages)
            .vertex_input_state(&pipeline_input_state)
            .input_assembly_state(&input_assembly)
            .rasterization_state(&raster_state)
            .color_blend_state(&blend_state)
            .multisample_state(&multisample_state)
            .viewport_state(&viewport_state)
            .depth_stencil_state(&depth_stencil_state)
            .dynamic_state(&dynamic_state)
            .layout(pipeline_layout.layout);

        if let Some(render_pass) = render_pass {
            pipeline_info = pipeline_info.render_pass(render_pass.handle)
        }

        let pipeline = unsafe {
            // panic_safety: if this is successful this should return 1 pipelines.
            device
                .create_graphics_pipelines(*cache, &[pipeline_info], None)
                .map_err(|e| e.1)?[0]
        };

        Ok(pipeline)
    }

    pub fn new(
        device: &Arc<ash::Device>,
        shader_assembly: &ShaderCompilerOutput<Vec<u32>>,
        reflection: &ShaderReflection,
        replicas: u32,
        render_pass_format: vk::Format,
        bypass_cache: bool,
    ) -> error::Result<VulkanGraphicsPipeline> {
        let pipeline_layout = PipelineLayoutObjects::new(reflection, replicas, device)?;

        let vertex_info =
            vk::ShaderModuleCreateInfo::default().code(shader_assembly.vertex.as_ref());
        let fragment_info =
            vk::ShaderModuleCreateInfo::default().code(shader_assembly.fragment.as_ref());

        let vertex_module = VulkanShaderModule::new(device, &vertex_info)?;
        let fragment_module = VulkanShaderModule::new(device, &fragment_info)?;

        let mut render_pass = None;
        let mut use_render_pass = false;
        if render_pass_format != vk::Format::UNDEFINED {
            render_pass = Some(VulkanRenderPass::create_render_pass(
                device,
                render_pass_format,
            )?);
            use_render_pass = true;
        }

        let (pipeline, pipeline_cache) = cache_pipeline(
            "vulkan",
            &[&shader_assembly.vertex, &shader_assembly.fragment],
            |pipeline_data| {
                let mut cache_info = vk::PipelineCacheCreateInfo::default();
                if let Some(pipeline_data) = pipeline_data.as_ref() {
                    cache_info = cache_info.initial_data(pipeline_data);
                }
                let cache_info = cache_info;

                let pipeline_cache = unsafe { device.create_pipeline_cache(&cache_info, None)? };

                let pipeline = Self::create_pipeline(
                    &device,
                    &pipeline_cache,
                    &pipeline_layout,
                    &vertex_module,
                    &fragment_module,
                    render_pass.as_ref(),
                )?;
                Ok::<_, FilterChainError>((pipeline, pipeline_cache))
            },
            |(_pipeline, cache)| unsafe { Ok(device.get_pipeline_cache_data(*cache)?) },
            bypass_cache,
        )?;

        let mut pipelines = FastHashMap::default();
        let mut render_passes = FastHashMap::default();

        pipelines.insert(render_pass_format, pipeline);
        render_passes.insert(render_pass_format, render_pass);

        Ok(VulkanGraphicsPipeline {
            device: Arc::clone(device),
            layout: pipeline_layout,
            pipelines,
            render_passes,
            vertex: vertex_module,
            fragment: fragment_module,
            cache: pipeline_cache,
            use_render_pass,
        })
    }

    pub(crate) fn recompile(&mut self, format: vk::Format) -> error::Result<()> {
        let new_renderpass = if self.use_render_pass {
            Some(VulkanRenderPass::create_render_pass(&self.device, format)?)
        } else {
            None
        };

        let new_pipeline = Self::create_pipeline(
            &self.device,
            &self.cache,
            &self.layout,
            &self.vertex,
            &self.fragment,
            new_renderpass.as_ref(),
        )?;

        self.render_passes.insert(format, new_renderpass);
        self.pipelines.insert(format, new_pipeline);

        Ok(())
    }
    #[inline(always)]
    pub(crate) fn begin_rendering(
        &self,
        output: &RenderTarget<OutputImage>,
        format: vk::Format,
        cmd: vk::CommandBuffer,
    ) -> error::Result<Option<vk::Framebuffer>> {
        if let Some(Some(render_pass)) = &self.render_passes.get(&format) {
            let attachments = [output.output.image_view];
            let framebuffer = unsafe {
                self.device.create_framebuffer(
                    &vk::FramebufferCreateInfo::default()
                        .render_pass(render_pass.handle)
                        .attachments(&attachments)
                        .width(output.output.size.width)
                        .height(output.output.size.height)
                        .layers(1),
                    None,
                )?
            };

            let clear_values = [vk::ClearValue {
                color: vk::ClearColorValue {
                    float32: [0.0, 0.0, 0.0, 0.0],
                },
            }];

            let render_pass_info = vk::RenderPassBeginInfo::default()
                .framebuffer(framebuffer)
                .render_pass(render_pass.handle)
                .clear_values(&clear_values)
                // always render into the full output, regardless of viewport settings.
                .render_area(vk::Rect2D {
                    offset: vk::Offset2D { x: 0, y: 0 },
                    extent: output.output.size.into(),
                });
            unsafe {
                self.device.cmd_begin_render_pass(
                    cmd,
                    &render_pass_info,
                    vk::SubpassContents::INLINE,
                );
            }
            Ok(Some(framebuffer))
        } else {
            let attachments = [vk::RenderingAttachmentInfo::default()
                .load_op(vk::AttachmentLoadOp::DONT_CARE)
                .store_op(vk::AttachmentStoreOp::STORE)
                .image_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
                .image_view(output.output.image_view)];

            let rendering_info = vk::RenderingInfo::default()
                .layer_count(1)
                .render_area(vk::Rect2D {
                    offset: vk::Offset2D { x: 0, y: 0 },
                    extent: output.output.size.into(),
                })
                .color_attachments(&attachments);

            unsafe {
                self.device.cmd_begin_rendering(cmd, &rendering_info);
            }
            Ok(None)
        }
    }

    pub(crate) fn end_rendering(&self, cmd: vk::CommandBuffer) {
        unsafe {
            if !self.use_render_pass {
                self.device.cmd_end_rendering(cmd);
            } else {
                self.device.cmd_end_render_pass(cmd)
            }
        }
    }
}

impl Drop for VulkanGraphicsPipeline {
    fn drop(&mut self) {
        unsafe {
            for (_, pipeline) in self.pipelines.iter_mut() {
                if *pipeline != vk::Pipeline::null() {
                    self.device.destroy_pipeline(*pipeline, None)
                }
            }

            if self.cache != vk::PipelineCache::null() {
                self.device.destroy_pipeline_cache(self.cache, None)
            }
        }
    }
}
