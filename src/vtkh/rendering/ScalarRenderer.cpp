#include "ScalarRenderer.hpp"
#include <vtkh/compositing/PayloadCompositor.hpp>

#include <vtkh/vtkh.hpp>

#include <vtkh/Logger.hpp>
#include <vtkh/utils/vtkm_array_utils.hpp>
#include <vtkh/utils/vtkm_dataset_info.hpp>
#include <vtkh/utils/PNGEncoder.hpp>
#include <vtkm/rendering/raytracing/Logger.h>
#include <vtkm/rendering/ScalarRenderer.h>

#ifdef VTKH_PARALLEL
  #include <mpi.h>
#endif
#include <assert.h>

namespace vtkh
{

namespace detail
{
vtkm::cont::DataSet
filter_scalar_fields(vtkm::cont::DataSet &dataset)
{
  vtkm::cont::DataSet res;
  const vtkm::Id num_coords = dataset.GetNumberOfCoordinateSystems();
  for(vtkm::Id i = 0; i < num_coords; ++i)
  {
    res.AddCoordinateSystem(dataset.GetCoordinateSystem(i));
  }
  res.SetCellSet(dataset.GetCellSet());

  const vtkm::Id num_fields = dataset.GetNumberOfFields();
  for(vtkm::Id i = 0; i < num_fields; ++i)
  {
    vtkm::cont::Field field = dataset.GetField(i);
    if(field.GetData().GetNumberOfComponents() == 1)
    {
      if(field.GetData().IsValueType<vtkm::Float32>() ||
         field.GetData().IsValueType<vtkm::Float64>())
      {
        res.AddField(field);
      }
    }
  }


  return res;
}

} // namespace detail

ScalarRenderer::ScalarRenderer()
  : m_width(1024),
    m_height(1024)
{
}

ScalarRenderer::~ScalarRenderer()
{
}

std::string
ScalarRenderer::GetName() const
{
  return "vtkh::ScalarRenderer";
}

void
ScalarRenderer::SetCamera(vtkmCamera &camera)
{
  m_camera = camera;
}

void
ScalarRenderer::PreExecute()
{
}

void
ScalarRenderer::Update()
{
  VTKH_DATA_OPEN(this->GetName());
#ifdef VTKH_ENABLE_LOGGING
  long long int in_cells = this->m_input->GetNumberOfCells();
  VTKH_DATA_ADD("input_cells", in_cells);
#endif
  PreExecute();
  DoExecute();
  PostExecute();
  VTKH_DATA_CLOSE();
}

void
ScalarRenderer::PostExecute()
{
  Filter::PostExecute();
}

void
ScalarRenderer::DoExecute()
{

  int num_domains = static_cast<int>(m_input->GetNumberOfDomains());
  this->m_output = new DataSet();

  //
  // There external faces + bvh construction happens
  // when we set the input for the renderer, which
  // we don't want to repeat for every camera. Also,
  // We could be processing AMR patches, numbering
  // in the 1000s, and with 100 images * 1000s amr
  // patches we could blow memory. We will set the input
  // once and composite after every image (todo: batch images
  // in groups of X).
  //
  std::vector<vtkm::rendering::ScalarRenderer> renderers;
  std::vector<vtkm::Id> cell_counts;
  renderers.resize(num_domains);
  cell_counts.resize(num_domains);
  for(int dom = 0; dom < num_domains; ++dom)
  {
    vtkm::cont::DataSet data_set;
    vtkm::Id domain_id;
    m_input->GetDomain(dom, data_set, domain_id);
    vtkm::cont::DataSet filtered = detail::filter_scalar_fields(data_set);
    renderers[dom].SetInput(filtered);
    renderers[dom].SetWidth(m_width);
    renderers[dom].SetHeight(m_height);

    // all the data sets better be the same
    cell_counts.push_back(data_set.GetCellSet().GetNumberOfCells());
  }

  // basic sanity checking
  int min_p = std::numeric_limits<int>::max();
  int max_p = std::numeric_limits<int>::min();
  bool do_once = true;

  std::vector<std::string> field_names;
  PayloadCompositor compositor;


  int num_cells;

  //Bounds needed for parallel execution
  float bounds[6]; 
  for(int dom = 0; dom < num_domains; ++dom)
  {
    vtkm::cont::DataSet data_set;
    vtkm::Id domain_id;
    m_input->GetDomain(dom, data_set, domain_id);
    num_cells = data_set.GetCellSet().GetNumberOfCells();

    if(data_set.GetCellSet().GetNumberOfCells())
    {
	
      Result res = renderers[dom].Render(m_camera);

      field_names = res.ScalarNames;
      PayloadImage *pimage = Convert(res);
      min_p = std::min(min_p, pimage->m_payload_bytes);
      max_p = std::max(max_p, pimage->m_payload_bytes);
      compositor.AddImage(*pimage);
      bounds[0] = pimage->m_bounds.X.Min;
      bounds[1] = pimage->m_bounds.X.Max;
      bounds[2] = pimage->m_bounds.Y.Min;
      bounds[3] = pimage->m_bounds.Y.Max;
      bounds[4] = pimage->m_bounds.Z.Min;
      bounds[5] = pimage->m_bounds.Z.Max;
      delete pimage;
    }
  }

  //Assume rank 0 has data, pass details to ranks with empty domains (no data).
#ifdef VTKH_PARALLEL
  MPI_Comm mpi_comm = MPI_Comm_f2c(vtkh::GetMPICommHandle());
  MPI_Bcast(bounds, 6, MPI_FLOAT, 0, mpi_comm);
  MPI_Bcast(&max_p, 1, MPI_INT, 0, mpi_comm);
  MPI_Bcast(&min_p, 1, MPI_INT, 0, mpi_comm);
#endif
  

  if(num_domains == 0 || num_cells == 0)
  {
    vtkm::Bounds b(bounds);
    PayloadImage p(b, max_p);
    int size = p.m_depths.size();
    float depths[size];
    for(int i = 0; i < size; i++)
      depths[i] = std::numeric_limits<int>::max();
    std::copy(depths, depths + size, &p.m_depths[0]);
    compositor.AddImage(p);
  }

  if(min_p != max_p)
  {
    throw Error("Scalar Renderer: mismatch in payload bytes");
  }
  PayloadImage final_image = compositor.Composite();
  if(vtkh::GetMPIRank() == 0)
  {
    Result final_result = Convert(final_image, field_names);
    vtkm::cont::DataSet dset = final_result.ToDataSet();
    const int domain_id = 0;
    this->m_output->AddDomain(dset, domain_id);
  }

}

ScalarRenderer::Result
ScalarRenderer::Convert(PayloadImage &image, std::vector<std::string> &names)
{
  Result result;
  result.ScalarNames = names;
  const int num_fields = names.size();

  const int dx  = image.m_bounds.X.Max - image.m_bounds.X.Min + 1;
  const int dy  = image.m_bounds.Y.Max - image.m_bounds.Y.Min + 1;
  const int size = dx * dy;

  result.Width = dx;
  result.Height = dy;

  std::vector<float*> buffers;
  for(int i = 0; i < num_fields; ++i)
  {
    vtkm::cont::ArrayHandle<vtkm::Float32> array;
    array.Allocate(size);
    result.Scalars.push_back(array);
    float* buffer = GetVTKMPointer(result.Scalars[i]);
    buffers.push_back(buffer);
  }

  const unsigned char *loads = &image.m_payloads[0];
  const size_t payload_size = image.m_payload_bytes;

  for(size_t x = 0; x < size; ++x)
  {
    for(int i = 0; i < num_fields; ++i)
    {
      const size_t offset = x * payload_size + i * sizeof(float);
      memcpy(&buffers[i][x], loads + offset, sizeof(float));
    }
  }

  //
  result.Depths.Allocate(size);
  float* dbuffer = GetVTKMPointer(result.Depths);
  memcpy(dbuffer, &image.m_depths[0], sizeof(float) * size);

  return result;
}

PayloadImage * ScalarRenderer::Convert(Result &result)
{
  const int num_fields = result.Scalars.size();
  const int payload_size = num_fields * sizeof(float);
  vtkm::Bounds bounds;
  bounds.X.Min = 1;
  bounds.Y.Min = 1;
  bounds.X.Max = result.Width;
  bounds.Y.Max = result.Height;
  const size_t size = result.Width * result.Height;

  PayloadImage *image = new PayloadImage(bounds, payload_size);
  unsigned char *loads = &image->m_payloads[0];

  float* dbuffer = GetVTKMPointer(result.Depths);
  memcpy(&image->m_depths[0], dbuffer, sizeof(float) * size);
  // copy scalars into payload
  std::vector<float*> buffers;
  for(int i = 0; i < num_fields; ++i)
  {
    vtkm::cont::ArrayHandle<vtkm::Float32> scalar = result.Scalars[i];
    float* buffer = GetVTKMPointer(scalar);
    buffers.push_back(buffer);
  }
#ifdef VTKH_USE_OPENMP
    #pragma omp parallel for
#endif
  for(size_t x = 0; x < size; ++x)
  {
    for(int i = 0; i < num_fields; ++i)
    {
      const size_t offset = x * payload_size + i * sizeof(float);
      memcpy(loads + offset, &buffers[i][x], sizeof(float));
    }
  }
  return image;
}

void
ScalarRenderer::SetHeight(const int height)
{
  m_height = height;
}

void
ScalarRenderer::SetWidth(const int width)
{
  m_width = width;
}

vtkh::DataSet *
ScalarRenderer::GetInput()
{
  return m_input;
}

} // namespace vtkh
